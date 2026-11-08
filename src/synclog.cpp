#include "synclog.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>

namespace {

// A run lists every changed file, which can be thousands after a resync;
// the window only needs enough to see what moved.
constexpr int kMaxChanges = 300;
constexpr int kMaxErrors = 30;

const QRegularExpression kLine(
    R"(^(\d{4}/\d\d/\d\d \d\d:\d\d:\d\d) (DEBUG|INFO|NOTICE|ERROR|CRITICAL)\s*: ?(.*)$)");
// bisync's "- Path2             File is new                                 - some/file".
const QRegularExpression kChange(R"(^- (\S+)\s+(.+?)\s+- (.+)$)");
const QRegularExpression kCounts(
    R"(^Path([12]):\s+\d+ changes?:\s+(\d+) new,\s+(\d+) modified,\s+(\d+) deleted)");
const QRegularExpression kListed(R"(^\s*Checks:.*Listed (\d+))");
const QRegularExpression kErrorPrefix(R"(^-\s+)");

// "2026/11/07 15:28:19", in local time, as a fixed offset from UTC. Qt
// resolves the system zone on every local-time conversion or comparison,
// about 0.2 ms each here: QDateTime::fromString on every line made reading
// the log take 14 seconds. A fixed offset costs nothing, and it is looked up
// once per day the log covers.
QDateTime toTime(const QString &stamp, QHash<QDate, int> &offsets) {
    const QDate date(stamp.mid(0, 4).toInt(), stamp.mid(5, 2).toInt(), stamp.mid(8, 2).toInt());
    const QTime time(stamp.mid(11, 2).toInt(), stamp.mid(14, 2).toInt(), stamp.mid(17, 2).toInt());
    auto offset = offsets.constFind(date);
    if (offset == offsets.cend())
        offset = offsets.insert(date, QDateTime(date, QTime(12, 0)).offsetFromUtc());
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(*offset));
}

bool isFailureLine(const QString &level, const QString &message) {
    return level == "ERROR" || level == "CRITICAL" || message.startsWith("Failed to bisync");
}

void addError(SyncRun &run, const QString &message) {
    QString text = message;
    text.remove(kErrorPrefix);
    text = text.trimmed();
    if (text.isEmpty() || run.errors.contains(text))
        return;
    if (run.errors.size() < kMaxErrors)
        run.errors.append(text);
}

// Same reading as drive-sync-aviso, so the window and the desktop
// notification name the same cause.
void classify(SyncRun &run) {
    for (const auto &error : run.errors)
        if (error.contains("cannot find prior Path1 or Path2 listings")) {
            run.causeKind = "lostListings";
            run.causeDetail.clear();
            return;
        }
    for (const auto &error : run.errors)
        if (error.contains("path1 and path2 are out of sync")) {
            run.causeKind = "outOfSync";
            run.causeDetail.clear();
            return;
        }
    // "Bisync aborted" and "Failed to bisync" only say that it stopped; the
    // line before them says why.
    for (const auto &error : run.errors)
        if (!error.startsWith("Bisync aborted") && !error.startsWith("Failed to bisync")) {
            run.causeKind = "error";
            run.causeDetail = error.left(200);
            return;
        }
    run.causeKind = run.errors.isEmpty() ? "none" : "error";
    run.causeDetail = run.errors.isEmpty() ? QString() : run.errors.first().left(200);
}

void finish(SyncRun &run, const QString &outcome, const QDateTime &when) {
    run.end = when;
    run.lastSeen = when;
    run.outcome = outcome;
    if (outcome == "failed")
        classify(run);
}

void addChange(SyncRun &run, const QString &tag, const QString &what, const QString &path) {
    QString kind;
    if (what == "File is new")
        kind = "new";
    else if (what.startsWith("File changed"))
        kind = "modified";
    else if (what == "File was deleted")
        kind = "deleted";
    else if (what.contains("both paths"))
        kind = "conflict";
    else
        return;  // "Queue copy to Path1" and the like repeat a change already counted.

    const QString side = kind == "conflict" ? "both" : tag == "Path1" ? "cloud" : tag == "Path2" ? "notebook" : QString();
    if (side.isEmpty())
        return;
    if (run.changes.size() < kMaxChanges)
        run.changes.append({side, kind, path.trimmed()});
    else
        ++run.changesOmitted;
}

}  // namespace

QList<SyncRun> SyncLog::parse(const QString &text, int maxRuns, bool startsMidway) {
    QList<SyncRun> runs;
    bool open = false;
    bool skipping = startsMidway;
    QString openStamp;  // The newest line of the open run, turned into lastSeen when needed.
    QHash<QDate, int> offsets;

    for (QString line : text.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        const auto match = kLine.match(line);
        if (!match.hasMatch()) {
            if (open && line.contains(QLatin1String("Listed "))) {
                const auto listed = kListed.match(line);
                if (listed.hasMatch())
                    runs.last().listed = listed.captured(1).toLongLong();
            }
            continue;
        }

        const QString stamp = match.captured(1);
        const QString level = match.captured(2);
        const QString message = match.captured(3).trimmed();

        if (message.startsWith("Synching Path1 ")) {
            // A run that never reached its end line was cut off: rclone was
            // killed, the notebook went down, or systemd hit TimeoutStartSec.
            if (open) {
                runs.last().outcome = "interrupted";
                runs.last().lastSeen = toTime(openStamp, offsets);
            }
            SyncRun run;
            run.start = toTime(stamp, offsets);
            run.lastSeen = run.start;
            openStamp = stamp;
            run.outcome = "open";
            run.phase = "listing";
            runs.append(run);
            open = true;
            skipping = false;
            continue;
        }
        if (skipping)
            continue;

        const bool failure = isFailureLine(level, message);
        if (!open) {
            if (!failure)
                continue;
            const QDateTime when = toTime(stamp, offsets);
            // rclone adds "Failed to bisync" after "Bisync aborted", so an
            // error right after a failed run belongs to it. Any other error
            // means rclone stopped before it started (no network, bad
            // config), which is a failed run of its own.
            const bool belongsToLast = !runs.isEmpty() && runs.last().outcome == "failed"
                                       && runs.last().end.isValid() && runs.last().end.secsTo(when) <= 60;
            if (!belongsToLast) {
                SyncRun run;
                run.start = when;
                runs.append(run);
            }
            addError(runs.last(), message);
            finish(runs.last(), "failed", belongsToLast ? runs.last().end : when);
            continue;
        }

        SyncRun &run = runs.last();
        openStamp = stamp;
        if (failure)
            addError(run, message);

        if (message == "Bisync successful") {
            finish(run, "success", toTime(stamp, offsets));
            open = false;
        } else if (message.startsWith("Bisync critical error") || message.startsWith("Bisync aborted")
                   || message.startsWith("Failed to bisync")) {
            finish(run, "failed", toTime(stamp, offsets));
            open = false;
        } else if (message.startsWith("Building Path1 and Path2 listings")) {
            run.phase = "listing";
        } else if (message.startsWith("Path1 checking for diffs") || message.startsWith("Path2 checking for diffs")) {
            run.phase = "comparing";
        } else if (message.startsWith("Applying changes")) {
            run.phase = "applying";
        } else if (message.startsWith("Copying Path2 files to Path1") || message.contains("Resync is copying files")) {
            run.resync = true;
            run.phase = "applying";
        } else if (message.contains("updating listings", Qt::CaseInsensitive) || message.startsWith("Validating listings")) {
            run.phase = "updating";
        } else if (message.startsWith(QLatin1String("Path")) && message.contains(QLatin1String(" changes"))) {
            const auto counts = kCounts.match(message);
            if (!counts.hasMatch())
                continue;
            SyncCounts &side = counts.captured(1) == "1" ? run.cloud : run.notebook;
            side.added = counts.captured(2).toInt();
            side.modified = counts.captured(3).toInt();
            side.deleted = counts.captured(4).toInt();
        } else if (!failure && message.startsWith(QLatin1String("- "))) {
            if (const auto change = kChange.match(message); change.hasMatch())
                addChange(run, change.captured(1), change.captured(2).trimmed(), change.captured(3));
        }
    }

    if (open)
        runs.last().lastSeen = toTime(openStamp, offsets);
    if (runs.size() > maxRuns)
        runs = runs.mid(runs.size() - maxRuns);
    return runs;
}

QString SyncLog::readTail(const QString &path, qint64 maxBytes, bool *found, bool *truncated) {
    QFile file(path);
    const bool readable = file.open(QIODevice::ReadOnly);
    if (found)
        *found = readable;
    if (truncated)
        *truncated = false;
    if (!readable)
        return {};

    const qint64 size = file.size();
    if (size > maxBytes) {
        file.seek(size - maxBytes);
        file.readLine();  // The rest of a line cut in half.
        if (truncated)
            *truncated = true;
    }
    return QString::fromUtf8(file.readAll());
}
