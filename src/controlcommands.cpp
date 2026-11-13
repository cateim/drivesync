#include "controlcommands.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <limits>
#include <unistd.h>

#include "fixstate.h"
#include "pausestate.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kCommandTimeoutMs = 15000;

// Answers the caller once, as JSON or as a line on err.
int answer(bool json, const QString &error, QTextStream &out, QTextStream &err) {
    if (json) {
        out << QJsonDocument(QJsonObject{{"ok", error.isEmpty()}, {"error", error}}).toJson(QJsonDocument::Compact)
            << "\n";
        out.flush();
    } else if (!error.isEmpty()) {
        err << "drivesync: " << error << "\n";
        err.flush();
    }
    return error.isEmpty() ? 0 : 1;
}

// Splits off "--json"; any other argument left in rest is for the caller.
bool takeJson(QStringList *arguments) { return arguments->removeAll("--json") > 0; }

bool runProgram(const QString &program, const QStringList &arguments, QString *error) {
    return runProcess(program, arguments, kCommandTimeoutMs, nullptr, error);
}

QStringList withLogFile(QStringList arguments, const QString &file) {
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == "--log-file" && i + 1 < arguments.size())
            arguments[++i] = file;
        else if (arguments.at(i).startsWith("--log-file="))
            arguments[i] = "--log-file=" + file;
    }
    return arguments;
}

}  // namespace

QString startOneRun(const BackendPaths &paths) {
    QString error;
    // --no-block: the run takes minutes; the state shows it once it starts.
    runProgram(paths.systemctl, {"--user", "start", "--no-block", "drive-sync.service"}, &error);
    return error;
}

int commandAnswer(bool json, const QString &error, QTextStream &out, QTextStream &err) {
    return answer(json, error, out, err);
}

QString takeoverLockPath(const QString &appState) { return appState + "/takeover-running"; }

bool takeoverLockHeld(const QString &appState, const QString &proc) {
    QFile lock(takeoverLockPath(appState));
    if (!lock.open(QIODevice::ReadOnly))
        return false;
    bool ok = false;
    const qint64 pid = lock.readAll().trimmed().toLongLong(&ok);
    QFile cmdline(QString("%1/%2/cmdline").arg(proc).arg(pid));
    if (!ok || pid <= 0 || !cmdline.open(QIODevice::ReadOnly))
        return false;
    // A SyncWatch window left open across the rename to DriveSync still runs
    // under its old name and may hold the lock.
    const QByteArray program = cmdline.readAll();
    return program.contains("drivesync") || program.contains("syncwatch");
}

bool runProcess(const QString &program, const QStringList &arguments, int timeoutMs, QByteArray *output,
                QString *error) {
    QString ignored;
    if (!error)
        error = &ignored;
    if (program.isEmpty()) {
        *error = "a program it needs was not found";
        return false;
    }
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs) || !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        *error = QString("%1 did not answer").arg(program);
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return true;
    *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (error->isEmpty())
        *error = QString("%1 exited with %2").arg(program).arg(process.exitCode());
    return false;
}

int runSyncCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = takeJson(&rest);
    if (!rest.isEmpty())
        return answer(json, "usage: drivesync sync [--json]", out, err);
    if (takeoverLockHeld(paths.appState, paths.proc))
        return answer(json, "a takeover is running; sync when it ends", out, err);
    // run-condition would skip the run anyway: say so instead of "ok".
    if (fixPresent(paths.appState))
        return answer(json, "a fix is under way, and it holds every run until it ends", out, err);
    // A pause that ended is cleared by the next run itself, so it does not stop one.
    const PauseMark mark = readPause(pauseFilePath(paths.appState));
    if (paths.managed && mark.present && !pauseEnded(mark, QDateTime::currentDateTimeUtc()))
        return answer(json, "drive-sync is paused; run drivesync resume first", out, err);
    return answer(json, startOneRun(paths), out, err);
}

int runPauseCommand(const QStringList &arguments, const BackendPaths &paths, const QDateTime &now, QTextStream &out,
                    QTextStream &err) {
    QStringList rest = arguments;
    const bool json = takeJson(&rest);
    const QString usage = "usage: drivesync pause [--minutes N | --until-tomorrow] [--json]";
    QDateTime until;
    if (rest.size() == 2 && rest.at(0) == "--minutes") {
        bool ok = false;
        const int minutes = rest.at(1).toInt(&ok);
        if (!ok || minutes <= 0)
            return answer(json, usage, out, err);
        until = now.addSecs(qint64(minutes) * 60);
    } else if (rest == QStringList{"--until-tomorrow"}) {
        until = nextMorning(now.toLocalTime());
    } else if (!rest.isEmpty()) {
        return answer(json, usage, out, err);
    }
    if (!paths.managed)
        return answer(json, "pausing needs DriveSync to run drive-sync; take it over first", out, err);

    const QString file = pauseFilePath(paths.appState);
    PauseMark mark = readPause(file);
    if (!mark.present || !mark.since.isValid())
        mark.since = now;  // a new end keeps when the pause began
    mark.present = true;
    mark.until = until;
    if (!writePause(file, mark))
        return answer(json, QString("cannot write %1").arg(file), out, err);
    return answer(json, QString(), out, err);
}

int runResumeCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = takeJson(&rest);
    if (!rest.isEmpty())
        return answer(json, "usage: drivesync resume [--json]", out, err);
    const QString file = pauseFilePath(paths.appState);
    if (!clearPause(file))
        return answer(json, QString("cannot remove %1").arg(file), out, err);
    // The pause is gone; a fix still holds the runs, and its end brings them back.
    if (fixPresent(paths.appState))
        return answer(json, QString(), out, err);
    return answer(json, startOneRun(paths), out, err);
}

int runConditionCommand(const BackendPaths &paths, const SyncConfig &config, const QDateTime &now) {
    // A fix holds every run from its first step to its end: its own unit
    // runs rclone, and a normal run in between would break the new base.
    if (fixPresent(paths.appState))
        return 1;
    if (!config.runOnlyWhen.isEmpty()) {
        // As systemd reads an ExecCondition: a crash or 255 is a failure,
        // any other non-zero exit a skip.
        QProcess check;
        check.start(QStandardPaths::findExecutable("sh"), {"-c", config.runOnlyWhen});
        if (!check.waitForStarted(kCommandTimeoutMs) || !check.waitForFinished(kCommandTimeoutMs)) {
            check.kill();
            check.waitForFinished(1000);
            return 255;
        }
        if (check.exitStatus() != QProcess::NormalExit || check.exitCode() == 255)
            return 255;
        if (check.exitCode() != 0)
            return 1;
    }
    const QString file = pauseFilePath(paths.appState);
    const PauseMark mark = readPause(file);
    if (!mark.present)
        return 0;
    if (pauseEnded(mark, now))
        // 1 would read as a deliberate skip, forever: say it failed instead.
        return clearPause(file) ? 0 : 255;
    return 1;
}

QStringList runCommandPlan(const SyncConfig &config, QString *refusal, const std::optional<AllowDeletes> &allow) {
    if (!filtersUsable(config.filtersFile, refusal))
        return {};
    const QStringList arguments = rcloneRunArguments(config);
    if (!allow)
        return arguments;
    // bisync checks each side on its own, so the side with more deletions sets it.
    const int largestSide = qMax(allow->cloudCount, allow->notebookCount);
    return withMaxDelete(arguments, maxDeletePercent(largestSide, allow->total, configuredMaxDelete(arguments)));
}

QString allowedRunLogLine(const AllowDeletes &allow, int percent) {
    return QString("DriveSync raised --max-delete to %1% for this run: %2 deletions confirmed")
        .arg(QString::number(percent), QString::number(allow.count));
}

void appendToLog(const QString &logFile, const QString &level, const QString &message) {
    QFile log(logFile);
    if (log.open(QIODevice::Append))
        log.write(QString("%1 %2: %3\n")
                      .arg(QDateTime::currentDateTime().toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), message)
                      .toUtf8());
}

QStringList runPlanTakingMark(const SyncConfig &config, const QString &appState, const QString &rclone,
                              const QDateTime &now, QString *refusal, QString *note) {
    note->clear();
    const std::optional<AllowDeletes> allow = takeAllowDeletes(appState, now);
    const QStringList normal = runCommandPlan(config, refusal);
    if (!allow || normal.isEmpty())
        return normal;
    const QString kept = QString("DriveSync kept --max-delete at %1% for this run: ").arg(configuredMaxDelete(normal));
    // The confirmation is for the files of one stopped run: that run must
    // still be the newest one (a mark written while the confirmed run was
    // already under way would otherwise serve a later run), and every
    // deletion the dry run finds must be in its list.
    const std::optional<BlockedDeletes> stopped =
        blockedDeletes(SyncLog::readRecent(config.logFile, kTailBytes), std::numeric_limits<int>::max());
    if (!stopped || !allow->runStart.isValid()
        || stopped->runStart.toSecsSinceEpoch() != allow->runStart.toSecsSinceEpoch()) {
        *note = kept + "a confirmation of deletions was dropped, since the run it was for is no longer the last one";
        return normal;
    }
    const QStringList raised = runCommandPlan(config, refusal, allow);
    const int percent = configuredMaxDelete(raised);
    const QString dryLog = appState + "/allowed-dry-run.log";
    QFile::remove(dryLog);
    QString error;
    // No time limit: the unit's TimeoutStartSec bounds the run, dry part included.
    const bool dryRunOk = runProcess(rclone, withLogFile(raised, dryLog) + QStringList{"--dry-run"}, -1, nullptr, &error);
    QFile dry(dryLog);
    const QString dryText = dry.open(QIODevice::ReadOnly) ? QString::fromUtf8(dry.readAll()) : QString();
    const BlockedDeletes found = deletionsInNewestRun(dryText, std::numeric_limits<int>::max());
    QSet<QString> confirmed;
    for (const BlockedDelete &d : stopped->list)
        confirmed.insert(QString(d.deletedInCloud ? "1:" : "2:") + d.path);
    int unconfirmed = 0;
    for (const BlockedDelete &d : found.list)
        if (!confirmed.contains(QString(d.deletedInCloud ? "1:" : "2:") + d.path))
            ++unconfirmed;
    if (dryRunOk && unconfirmed == 0) {
        *note = allowedRunLogLine(*allow, percent);
        return raised;
    }
    if (dryRunOk)
        *note = kept
                + QString("a dry run finds %1 deletion%2 not in the list that was confirmed (gone from the cloud: %3, "
                          "gone from the notebook: %4)")
                      .arg(QString::number(unconfirmed), unconfirmed == 1 ? QString() : QString("s"),
                           QString::number(found.cloudCount), QString::number(found.notebookCount));
    else if (dryText.contains("Safety abort: too many deletes"))
        *note = kept
                + QString("its dry run with the raised guard still stopped, so more files would go than the %1 confirmed")
                      .arg(allow->count);
    else
        *note = kept + QString("its dry run with the raised guard failed (%1)").arg(error.left(200));
    return normal;
}

int runRunCommand(const BackendPaths &paths, QTextStream &err) {
    QString error;
    const auto config = loadSyncConfig(paths.config, &error);
    const QString rclone = QStandardPaths::findExecutable("rclone");
    QStringList arguments;
    QString note;
    if (config && rclone.isEmpty())
        error = "rclone was not found";
    else if (config)
        arguments = runPlanTakingMark(*config, paths.appState, rclone, QDateTime::currentDateTimeUtc(), &error, &note);
    if (arguments.isEmpty()) {
        err << "drivesync: did not start rclone: " << error << "\n";
        err.flush();
        // In the log too, in rclone's format, so the window and the
        // notification show why this run failed.
        appendToLog(config ? config->logFile : paths.log, "CRITICAL", "DriveSync did not start rclone: " + error);
        return 1;
    }
    if (!note.isEmpty())
        appendToLog(config->logFile, "INFO", note);

    QList<QByteArray> storage{QFile::encodeName(rclone)};
    for (const QString &argument : arguments)
        storage << argument.toUtf8();
    QList<char *> argv;
    for (QByteArray &item : storage)
        argv << item.data();
    argv << nullptr;
    execv(argv.first(), argv.data());
    err << "drivesync: could not start " << rclone << "\n";
    err.flush();
    return 1;
}
