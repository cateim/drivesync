#include "blockeddeletes.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringView>
#include <QTimeZone>
#include <limits>

#include "controlcommands.h"
#include "fixcommand.h"
#include "fixstate.h"
#include "listingdiff.h"
#include "pausestate.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kAllowFileVersion = 1;
constexpr qint64 kAllowSeconds = 30 * 60;
constexpr int kBisyncDefaultMaxDelete = 50;  // rclone 1.75's --max-delete default for bisync

// "2026/11/12 18:41:19 INFO  : - Path2             File was deleted                            - a/b.txt".
// The path keeps what follows "- " whole: a leading space is part of it.
const QRegularExpression kDeleted(
    R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d [A-Z]+\s*: - Path([12])\s+File was deleted\s+- (.*)$)");
const QRegularExpression kSynching(R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d [A-Z]+\s*: Synching Path1 )");
const QRegularExpression kGuard(R"(too many deletes \(>\d+%, (\d+) of (\d+)\))");

// Where the newest run starts: its "Synching Path1" line, or -1. Searched
// from the end for the text, then checked as a whole line, so a file named
// like it is never taken for one.
qsizetype newestRunStart(const QString &text) {
    qsizetype from = text.size();
    while (from > 0) {
        const qsizetype hit = text.lastIndexOf(QLatin1String(": Synching Path1 "), from - 1);
        if (hit < 0)
            return -1;
        const qsizetype start = text.lastIndexOf('\n', hit) + 1;
        if (kSynching.matchView(QStringView(text).mid(start, hit - start + 17)).hasMatch())
            return start;
        from = hit;
    }
    return -1;
}

// rclone writes a name with " or \ in it the Go way, quotes included.
QString unquoted(const QString &path) {
    if (path.size() < 2 || !path.startsWith('"') || !path.endsWith('"'))
        return path;
    bool ok = false, notUtf8 = false;
    const QString name = unquoteGo(path.mid(1, path.size() - 2), &ok, &notUtf8);
    return ok ? name : path;
}

}  // namespace

BlockedDeletes deletionsInNewestRun(const QString &logText, int limit) {
    BlockedDeletes result;
    const qsizetype start = newestRunStart(logText);
    if (start < 0)
        return result;
    QList<BlockedDelete> cloud, notebook;
    for (QStringView line : QStringView(logText).mid(start).split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.contains(QLatin1String("File was deleted")))
            continue;
        const QRegularExpressionMatch m = kDeleted.matchView(line);
        if (!m.hasMatch())
            continue;
        const bool inCloud = m.capturedView(1) == QLatin1String("1");
        ++result.count;
        ++(inCloud ? result.cloudCount : result.notebookCount);
        QList<BlockedDelete> &side = inCloud ? cloud : notebook;
        if (side.size() < limit)
            side.append({unquoted(m.captured(2)), inCloud});
    }
    result.list = (cloud + notebook).mid(0, qMax(0, limit));
    return result;
}

bool guardStopHolds(const QList<SyncRun> &runs) {
    for (auto it = runs.crbegin(); it != runs.crend(); ++it) {
        if (it->outcome == "open" || it->phase.isEmpty())
            continue;
        return it->outcome == "failed" && it->causeKind == "maxDelete";
    }
    return false;
}

std::optional<BlockedDeletes> blockedDeletes(const QString &logText, int limit) {
    const qsizetype start = newestRunStart(logText);
    if (start < 0)
        return std::nullopt;
    // The parser also makes a run of an error with no run open (DriveSync's
    // refusal, rclone that did not start), so one run means nothing came after.
    const QList<SyncRun> runs = SyncLog::parse(logText.mid(start));
    if (runs.size() != 1 || runs.first().outcome != "failed" || runs.first().causeKind != "maxDelete")
        return std::nullopt;
    BlockedDeletes result = deletionsInNewestRun(logText, limit);
    result.runStart = runs.first().start;
    const QRegularExpressionMatch guard = kGuard.match(runs.first().causeDetail);
    result.guardCount = guard.captured(1).toInt();
    result.guardTotal = guard.captured(2).toInt();
    return result;
}

QString allowDeletesPath(const QString &appState) { return appState + "/allow-deletes.json"; }

bool writeAllowDeletes(const QString &appState, const AllowDeletes &allow) {
    if (!QDir().mkpath(appState))
        return false;
    QSaveFile out(allowDeletesPath(appState));
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(QJsonObject{{"version", kAllowFileVersion},
                                        {"count", allow.count},
                                        {"cloudCount", allow.cloudCount},
                                        {"notebookCount", allow.notebookCount},
                                        {"total", allow.total},
                                        {"at", allow.at.isValid() ? allow.at.toSecsSinceEpoch() : 0},
                                        {"runStart", allow.runStart.isValid() ? allow.runStart.toSecsSinceEpoch() : 0}})
                  .toJson(QJsonDocument::Compact));
    return out.commit();
}

std::optional<AllowDeletes> takeAllowDeletes(const QString &appState, const QDateTime &now) {
    QFile in(allowDeletesPath(appState));
    if (!in.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
    in.close();
    // Gone before anything else: a mark that could not be removed would
    // serve a second run.
    if (!in.remove())
        return std::nullopt;
    const QJsonObject object = document.object();
    AllowDeletes allow;
    allow.count = object.value("count").toInt();
    allow.cloudCount = object.value("cloudCount").toInt();
    allow.notebookCount = object.value("notebookCount").toInt();
    allow.total = object.value("total").toInt();
    const qint64 at = object.value("at").toInteger();
    allow.at = QDateTime::fromSecsSinceEpoch(at, QTimeZone::UTC);
    const qint64 runStart = object.value("runStart").toInteger();
    if (runStart > 0)
        allow.runStart = QDateTime::fromSecsSinceEpoch(runStart, QTimeZone::UTC);
    // A clock set back since then makes the age negative: refused too, the
    // safer side.
    const qint64 age = now.toSecsSinceEpoch() - at;
    if (object.value("version").toInt() != kAllowFileVersion || at <= 0 || age < 0 || age > kAllowSeconds
        || allow.count <= 0 || allow.total <= 0)
        return std::nullopt;
    return allow;
}

int maxDeletePercent(int largestSide, int total, int configured) {
    const qint64 percent = total > 0 ? (qint64(largestSide) * 100 + total - 1) / total : 100;
    return int(qMin<qint64>(100, qMax<qint64>(configured, percent)));
}

QStringList withMaxDelete(const QStringList &arguments, int percent) {
    QStringList result = arguments;
    bool found = false;
    for (qsizetype i = 0; i < result.size(); ++i) {
        if (result.at(i) == "--max-delete" && i + 1 < result.size()) {
            result[++i] = QString::number(percent);
            found = true;
        } else if (result.at(i).startsWith("--max-delete=")) {
            result[i] = QString("--max-delete=%1").arg(percent);
            found = true;
        }
    }
    if (!found)
        result << "--max-delete" << QString::number(percent);
    return result;
}

namespace {

constexpr int kListLimit = 500;

QJsonObject listJson(const std::optional<BlockedDeletes> &blocked) {
    const BlockedDeletes b = blocked.value_or(BlockedDeletes());
    QJsonArray list;
    for (const BlockedDelete &d : b.list)
        list.append(QJsonObject{{"path", d.path}, {"deletedInCloud", d.deletedInCloud}});
    return QJsonObject{{"ok", true},
                       {"error", QString()},
                       {"count", b.count},
                       {"cloudCount", b.cloudCount},
                       {"notebookCount", b.notebookCount},
                       {"guardCount", b.guardCount},
                       {"guardTotal", b.guardTotal},
                       {"runStart", b.runStart.isValid() ? b.runStart.toSecsSinceEpoch() : 0},
                       {"list", list}};
}

void printList(const std::optional<BlockedDeletes> &blocked, QTextStream &out) {
    if (!blocked) {
        out << "Nothing is held back: the last run did not stop at the max-delete guard.\n";
        return;
    }
    out << QString("The last run stopped at the max-delete guard (%1 of %2 on one side). It would delete %3 files:\n")
               .arg(QString::number(blocked->guardCount), QString::number(blocked->guardTotal),
                    QString::number(blocked->count));
    out << QString("  %1 gone from the cloud: deleted on the notebook, for good\n").arg(blocked->cloudCount);
    out << QString("  %1 gone from the notebook: sent to the Drive trash\n").arg(blocked->notebookCount);
    bool cloudHeader = false, notebookHeader = false;
    for (const BlockedDelete &d : blocked->list) {
        bool &header = d.deletedInCloud ? cloudHeader : notebookHeader;
        if (!header)
            out << (d.deletedInCloud ? "Gone from the cloud:\n" : "Gone from the notebook:\n");
        header = true;
        out << "  " << d.path << "\n";
    }
    if (blocked->list.size() < blocked->count)
        out << QString("(the first %1 of %2)\n").arg(blocked->list.size()).arg(blocked->count);
    out << QString("To let one run delete them: drivesync deletes --allow %1\n").arg(blocked->count);
}

// The checks, the mark, then the run that uses it.
QString allowDeletes(const BackendPaths &paths, const std::optional<BlockedDeletes> &blocked, int count) {
    if (!paths.managed)
        return "letting deletions through needs DriveSync to run drive-sync; take it over first";
    if (takeoverLockHeld(paths.appState, paths.proc))
        return "a takeover is running; try again when it ends";
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const PauseMark pause = readPause(pauseFilePath(paths.appState));
    if (pause.present && !pauseEnded(pause, now))
        return "drive-sync is paused; run drivesync resume first";
    if (!blocked)
        return "nothing is held back: the last run did not stop at the max-delete guard";
    if (blocked->guardTotal <= 0)
        return "the max-delete line of the last run cannot be read";
    if (count != blocked->count)
        return QString("the last run would delete %1 files, not %2; see them with drivesync deletes --list")
            .arg(blocked->count)
            .arg(count);
    AllowDeletes allow;
    allow.count = blocked->count;
    allow.cloudCount = blocked->cloudCount;
    allow.notebookCount = blocked->notebookCount;
    allow.total = blocked->guardTotal;
    allow.at = now;
    allow.runStart = blocked->runStart;

    // Under the fix's lock: a fix starting meanwhile would hold the run.
    const FixLock lock(paths.appState);
    const bool fixing = fixPresent(paths.appState);
    const FixState fix = fixing ? readFix(paths.appState) : FixState();
    // Only a fix whose final run stopped at the guard: the confirmation is
    // for that run, so the fix goes back to it.
    if (fixing && !(fix.step == "failed" && fix.failedStep == "finishing" && fix.error.contains("too many deletes")))
        return "a fix is under way, and it holds every run until it ends";
    if (!writeAllowDeletes(paths.appState, allow))
        return QString("cannot write %1").arg(allowDeletesPath(paths.appState));
    const QString error = fixing ? retryFix(paths, fix) : startOneRun(paths);
    if (!error.isEmpty())
        QFile::remove(allowDeletesPath(paths.appState));  // no later run may take it
    return error;
}

}  // namespace

int runDeletesCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    const bool list = rest == QStringList{"--list"};
    bool countOk = false;
    const int count = rest.size() == 2 && rest.at(0) == "--allow" ? rest.at(1).toInt(&countOk) : 0;
    if (!list && (!countOk || count <= 0))
        return commandAnswer(json, "usage: drivesync deletes --list | --allow <count> [--json]", out, err);
    // The window shows 500 paths; printed here, every one of them is.
    const auto blocked = blockedDeletes(SyncLog::readRecent(paths.log, kTailBytes),
                                        list && !json ? std::numeric_limits<int>::max() : kListLimit);
    if (!list)
        return commandAnswer(json, allowDeletes(paths, blocked, count), out, err);
    if (json)
        out << QJsonDocument(listJson(blocked)).toJson(QJsonDocument::Compact) << "\n";
    else
        printList(blocked, out);
    out.flush();
    return 0;
}

int configuredMaxDelete(const QStringList &arguments) {
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == "--max-delete" && i + 1 < arguments.size())
            return arguments.at(i + 1).toInt();
        if (arguments.at(i).startsWith("--max-delete="))
            return arguments.at(i).mid(13).toInt();
    }
    return kBisyncDefaultMaxDelete;
}
