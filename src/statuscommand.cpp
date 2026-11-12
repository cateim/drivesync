#include "statuscommand.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>

#include "conflictcache.h"
#include "synclog.h"

namespace {

// A cold find over a large Drive can take a while; it only runs when the
// cached count is older than kConflictCacheSeconds.
constexpr int kFindTimeoutMs = 30000;

// Runs a program and waits. A timeout kills it, so a stuck child never
// outlives the command.
bool runAndWait(const QString &program, const QStringList &arguments, int timeoutMs, QByteArray *output) {
    if (program.isEmpty())
        return false;
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs))
        return false;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    *output = process.readAllStandardOutput();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

int countConflicts(const BackendPaths &paths, const QDateTime &now) {
    const QString cacheFile = paths.appState.isEmpty() ? QString() : paths.appState + "/conflicts.json";
    if (!cacheFile.isEmpty()) {
        const ConflictCount cached = readConflictCache(cacheFile);
        if (conflictCacheFresh(cached, now))
            return cached.count;
    }
    QByteArray output;
    const bool ok = runAndWait(paths.find, conflictFindArguments(paths.drive), kFindTimeoutMs, &output);
    // find exits 1 when one folder can't be read but still lists the rest,
    // so only an empty answer from a failed run is an error.
    int count = 0;
    for (const QByteArray &line : output.split('\n'))
        if (line.count('\t') >= 2)
            ++count;
    QString error;
    if (paths.find.isEmpty())
        error = "noFind";
    else if (!ok && output.isEmpty())
        error = "scanFailed";
    if (!cacheFile.isEmpty())
        writeConflictCache(cacheFile, {count, error, now});
    return count;
}

}  // namespace

SnapshotSources gatherSources(const BackendPaths &paths, const QDateTime &now, int timeoutMs, QDateTime *nextRun) {
    SnapshotSources src;
    src.now = now;

    bool found = false;
    bool truncated = false;
    const QString text = SyncLog::readTail(paths.log, kTailBytes, &found, &truncated);
    src.logFound = found;
    src.runs = SyncLog::parse(text, kStateRuns, truncated);

    QByteArray show;
    const bool showOk = runAndWait(paths.systemctl, systemdShowArguments(), timeoutMs, &show);
    const auto units = Backend::parseShow(show);
    src.systemctlWorks = showOk && units.size() == 3;
    if (src.systemctlWorks) {
        src.service = units.at(0);
        src.timer = units.at(1);
        src.fixUnitActive = fixUnitBusy(units.at(2));
        QByteArray timers;
        if (nextRun && runAndWait(paths.systemctl, listTimersArguments(), timeoutMs, &timers))
            *nextRun = Backend::parseNextElapse(timers);
    }

    src.resyncRunning = Backend::resyncRunning(paths.stateDir, paths.proc);
    src.driveFound = QFileInfo(paths.drive).isDir();
    src.conflicts = src.driveFound ? countConflicts(paths, now) : 0;
    src.managed = paths.managed;
    if (!paths.appState.isEmpty()) {
        src.pause = readPause(pauseFilePath(paths.appState));
        src.fixPresent = fixPresent(paths.appState);
        if (src.fixPresent)
            src.fix = readFix(paths.appState);
    }
    return src;
}

int runStatusCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err,
                     int timeoutMs) {
    bool json = false;
    for (const QString &argument : arguments) {
        if (argument != "--json") {
            err << usageText();
            err.flush();
            return 1;
        }
        json = true;
    }

    // UTC for the same reason as Backend::recompute: local-time comparisons
    // make Qt resolve the zone on every call.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime nextRun;
    const Snapshot snapshot = buildSnapshot(gatherSources(paths, now, timeoutMs, &nextRun));
    if (json) {
        out << QJsonDocument(snapshotToJson(snapshot, nextRun, now)).toJson(QJsonDocument::Compact) << "\n";
    } else {
        out << snapshot.summary.status;
        if (!snapshot.summary.warnings.isEmpty())
            out << " (warnings: " << snapshot.summary.warnings.join(", ") << ")";
        out << "\n";
    }
    out.flush();
    return 0;
}

QString usageText() {
    return "usage: drivesync [open]\n"
           "       drivesync status [--json]\n"
           "       drivesync sync [--json]\n"
           "       drivesync pause [--minutes N | --until-tomorrow] [--json]\n"
           "       drivesync resume [--json]\n"
           "       drivesync takeover --check | --apply | --undo [--json]\n"
           "       drivesync fix --start | --confirm | --answers <file.json> | --cancel | --retry [--json]\n"
           "       drivesync run-condition | run | notify | watchdog | fix-run    (for the units)\n"
           "       drivesync --help\n";
}
