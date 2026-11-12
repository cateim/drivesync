#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>
#include <algorithm>
#include <functional>

#include "backend.h"
#include "clockformat.h"
#include "conflictcache.h"
#include "controlcommands.h"
#include "legacymigration.h"
#include "notifier.h"
#include "pausestate.h"
#include "pluginsetup.h"
#include "snapshot.h"
#include "statuscommand.h"
#include "syncconfig.h"
#include "synclog.h"
#include "takeover.h"
#include "takeoversetup.h"
#include "theme.h"
#include "translator.h"

namespace {

// One line the way rclone writes it: "2026/11/07 15:28:19 INFO  : message".
QString line(const QDateTime &when, const QString &level, const QString &message) {
    return QString("%1 %2: %3\n").arg(when.toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), message);
}

QString change(const QString &tag, const QString &what, const QString &path) {
    return QString("- %1%2 - %3").arg(tag.leftJustified(18), what.leftJustified(43), path);
}

// A normal run that copied one file each way, as drive-sync logs it.
QString successfulRun(const QDateTime &start, int seconds) {
    QString log;
    log += line(start, "INFO", "Bisyncing with Comparison Settings: ");
    log += "{\n\t\"Modtime\": true,\n}\n";
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "INFO", "Building Path1 and Path2 listings");
    log += line(start.addSecs(60), "INFO", "");
    log += "Transferred:   \t          0 B / 0 B, -, 0 B/s, ETA -\nChecks:             14560 / 14560, 100%, Listed 16680\nElapsed time:        59.9s\n\n";
    log += line(start.addSecs(seconds - 20), "INFO", "Path1 checking for diffs");
    log += line(start.addSecs(seconds - 20), "INFO", change("Path1", "File changed: size (larger), time (newer)", "Notes/todo.md"));
    log += line(start.addSecs(seconds - 20), "INFO", "Path1:    1 changes:    0 new,    1 modified,    0 deleted");
    log += line(start.addSecs(seconds - 20), "INFO", "Path2 checking for diffs");
    log += line(start.addSecs(seconds - 20), "INFO", change("Path2", "File is new", "GitHub/app/README.md"));
    log += line(start.addSecs(seconds - 20), "INFO", "Path2:    1 changes:    1 new,    0 modified,    0 deleted");
    log += line(start.addSecs(seconds - 19), "INFO", "Applying changes");
    log += line(start.addSecs(seconds - 19), "INFO", change("Path2", "Queue copy to Path1", "gdrive{x}:/GitHub/app/README.md"));
    log += line(start.addSecs(seconds - 2), "INFO", "Updating listings");
    log += line(start.addSecs(seconds), "INFO", "Bisync successful");
    return log;
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A home the way SyncWatch left it after a takeover: config with its filters
// and .md5, a pause mark, and one backup whose manifest names its own folder.
struct LegacyHome {
    QString home;
    QByteArray filters = "- .git/**\n- node_modules/**\n";
};

bool makeLegacyHome(LegacyHome &legacy) {
    const QString config = legacy.home + "/.config/syncwatch";
    const QString backup = legacy.home + "/.local/share/syncwatch/taken-over/20261110-224919";
    if (!QDir().mkpath(config) || !QDir().mkpath(legacy.home + "/.local/state/syncwatch")
        || !QDir().mkpath(backup + "/bin"))
        return false;
    SyncConfig sync;
    sync.remote = "gdrive:";
    sync.localPath = legacy.home + "/Drive";
    sync.workDir = legacy.home + "/.local/state/rclone/bisync";
    sync.filtersFile = config + "/filters.txt";
    sync.logFile = legacy.home + "/.local/state/drive-sync.log";
    sync.rcloneFlags = {"--resilient"};
    QString error;
    const QJsonObject manifest{
        {"version", 1},
        {"moved", QJsonArray{QJsonObject{{"from", legacy.home + "/.local/bin/drive-sync.sh"},
                                         {"to", backup + "/bin/drive-sync.sh"}}}},
        {"undone", false}};
    return saveSyncConfig(config + "/config.json", sync, &error) && write(sync.filtersFile, legacy.filters)
           && write(sync.filtersFile + ".md5",
                    QCryptographicHash::hash(legacy.filters, QCryptographicHash::Md5).toHex())
           && write(legacy.home + "/.local/state/syncwatch/paused.json", "{}")
           && write(backup + "/bin/drive-sync.sh", "#!/bin/sh\n")
           && write(backup + "/manifest.json", QJsonDocument(manifest).toJson());
}

// A systemctl that answers like systemd with the service idle and the timer on.
QString fakeSystemctl(const QString &dir, const QDateTime &serviceStart, const QString &serviceActive = "inactive") {
    const QString path = dir + "/systemctl";
    const QString script = QString(
        "#!/bin/sh\n"
        "case \"$*\" in\n"
        "  *list-timers*) echo '[{\"next\":%1,\"unit\":\"drive-sync.timer\"}]' ;;\n"
        "  *) printf 'LoadState=loaded\\nActiveState=%2\\nUnitFileState=static\\nResult=success\\n"
        "ExecMainStartTimestamp=@%3\\n\\nLoadState=loaded\\nActiveState=active\\nUnitFileState=enabled\\nResult=success\\n' ;;\n"
        "esac\n")
        .arg(QDateTime::currentDateTime().addSecs(420).toMSecsSinceEpoch() * 1000)
        .arg(serviceActive)
        .arg(serviceStart.toSecsSinceEpoch());
    write(path, script.toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

SyncInputs healthy() {
    SyncInputs in;
    in.serviceLoad = "loaded";
    in.serviceActive = "inactive";
    in.serviceResult = "success";
    in.timerActive = "active";
    in.timerFileState = "enabled";
    in.now = QDateTime::currentDateTime();
    in.serviceStart = in.now.addSecs(-600);
    in.lastStart = in.serviceStart;
    in.lastOutcome = "success";
    in.lastSuccess = in.now.addSecs(-120);
    return in;
}

// A run that stopped on lost listings, as drive-sync logs it.
QString failedRun(const QDateTime &start) {
    QString log;
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
    log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
    log += line(start.addSecs(1), "NOTICE", "Failed to bisync: bisync aborted");
    return log;
}

// A fake omarchy and omarchy-shell that append every call to calls.log, and
// answer "plugin list --json" with list.json, all inside dir.
PluginPaths fakeOmarchy(const QString &dir, const QByteArray &list) {
    write(dir + "/list.json", list);
    const QString omarchy = QString(
        "#!/bin/sh\n"
        "echo \"$*\" >> '%1/calls.log'\n"
        "case \"$*\" in\n"
        "  'plugin list --json') cat '%1/list.json' ;;\n"
        "esac\n").arg(dir);
    write(dir + "/omarchy", omarchy.toUtf8());
    write(dir + "/omarchy-shell", QString("#!/bin/sh\necho \"shell-cmd $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    for (const QString &name : {QString("omarchy"), QString("omarchy-shell")})
        QFile::setPermissions(dir + "/" + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QDir(dir).mkpath("shipped");
    return {dir + "/shipped", dir + "/plugins", dir + "/omarchy", dir + "/omarchy-shell"};
}

QStringList calls(const QString &dir) {
    QFile file(dir + "/calls.log");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

// What buildSnapshot sees with the service idle, the timer on and enabled,
// and the log already parsed into runs.
SnapshotSources idleSources(const QString &log, const QDateTime &now) {
    SnapshotSources src;
    src.runs = SyncLog::parse(log);
    src.logFound = true;
    src.service = {{"LoadState", "loaded"}, {"ActiveState", "inactive"}, {"Result", "success"},
                   {"ExecMainStartTimestamp", QString("@%1").arg(now.addSecs(-100).toSecsSinceEpoch())}};
    src.timer = {{"ActiveState", "active"}, {"UnitFileState", "enabled"}};
    src.now = now;
    return src;
}

// The ExecStart of the unit drive-sync.sh writes, with the home made up.
constexpr char kOldExecStart[] =
    "/usr/bin/rclone bisync gdrive: /home/someone/Drive --workdir /home/someone/.local/state/rclone/bisync "
    "--filters-file /home/someone/.config/rclone/gdrive-filtros.txt --drive-skip-gdocs --drive-skip-shortcuts "
    "--resilient --recover --max-lock 2m --max-delete 5 --conflict-resolve newer "
    "--log-file /home/someone/.local/state/drive-sync.log --log-level INFO";

// What a takeover of that unit writes.
SyncConfig oldUnitConfig() {
    SyncConfig config;
    config.remote = "gdrive:";
    config.localPath = "/home/someone/Drive";
    config.workDir = "/home/someone/.local/state/rclone/bisync";
    config.filtersFile = "/home/someone/.config/drivesync/filters.txt";
    config.logFile = "/home/someone/.local/state/drive-sync.log";
    config.rcloneFlags = {"--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                          "--max-lock", "2m", "--max-delete", "5", "--conflict-resolve", "newer",
                          "--log-level", "INFO"};
    config.runOnlyWhen = "findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'";
    return config;
}

// A systemctl that appends every call to calls.log and exits with exitCode,
// printing "denied" on stderr when it fails.
QString recordingSystemctl(const QString &dir, int exitCode = 0) {
    const QString path = dir + "/systemctl";
    write(path, QString("#!/bin/sh\necho \"systemctl $*\" >> '%1/calls.log'\n"
                        "[ %2 -eq 0 ] || echo denied >&2\nexit %2\n").arg(dir).arg(exitCode).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Paths for the control commands, everything inside dir.
BackendPaths controlPaths(const QString &dir, bool managed, int systemctlExit = 0) {
    BackendPaths paths{dir + "/drive-sync.log", dir + "/Drive", dir + "/state", dir + "/proc",
                       recordingSystemctl(dir, systemctlExit), QString(), dir + "/app"};
    paths.managed = managed;
    return paths;
}

// notify-send, gdbus and omarchy that append each call to calls.log. The
// notify-send prints the next id from ids.txt, or nothing when it is "none"
// (no graphical session).
NotifierPaths fakeNotifier(const QString &dir) {
    write(dir + "/ids.txt", "41\n");
    const QString notify = QString(
        "#!/bin/sh\n"
        "echo \"notify-send $*\" >> '%1/calls.log'\n"
        "id=$(cat '%1/ids.txt')\n"
        "[ \"$id\" = none ] && exit 0\n"
        "echo $((id + 1)) > '%1/ids.txt'\n"
        "echo $id\n").arg(dir);
    write(dir + "/notify-send", notify.toUtf8());
    write(dir + "/gdbus", QString("#!/bin/sh\necho \"gdbus $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    write(dir + "/omarchy", QString("#!/bin/sh\necho \"omarchy $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    for (const QString &name : {QString("notify-send"), QString("gdbus"), QString("omarchy")})
        QFile::setPermissions(dir + "/" + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return {dir + "/notify-send", dir + "/gdbus", dir + "/omarchy", QLocale::c()};
}

// A systemctl for the watchdog: LoadState, is-active and is-enabled answers.
QString watchdogSystemctl(const QString &dir, const QString &load, const QString &active, const QString &enabled) {
    const QString path = dir + "/systemctl";
    write(path, QString("#!/bin/sh\n"
                        "case \"$*\" in\n"
                        "  *LoadState*) echo %1 ;;\n"
                        "  *is-active*) echo %2; [ %2 = active ] ;;\n"
                        "  *is-enabled*) echo %3; [ %3 = enabled ] ;;\n"
                        "esac\n").arg(load, active, enabled).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QStringList notifications(const QString &dir) {
    QStringList sent;
    for (const QString &call : calls(dir))
        if (call.startsWith("notify-send"))
            sent << call;
    return sent;
}

// A drive-sync set up the way drive-sync.sh does it, inside root/home, with
// a systemctl and an rclone that record their calls in root/calls.log.
struct OldSetup {
    QString home;
    TakeoverPaths paths;
    QString filters;
};

QString oldServiceText(const QString &home) {
    return QString("[Unit]\nDescription=rclone bisync de ~/Drive com o Google Drive\n\n[Service]\nType=oneshot\n"
                   "# Rodada travada vira falha.\nTimeoutStartSec=2h\n"
                   "ExecCondition=/usr/bin/sh -c \"findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'\"\n"
                   "ExecStart=/usr/bin/rclone bisync gdrive: %1/Drive --workdir %1/.local/state/rclone/bisync "
                   "--filters-file %1/.config/rclone/gdrive-filtros.txt --drive-skip-gdocs --drive-skip-shortcuts "
                   "--resilient --recover --max-lock 2m --max-delete 5 --conflict-resolve newer "
                   "--log-file %1/.local/state/drive-sync.log --log-level INFO\n"
                   "ExecStopPost=-%1/.local/bin/drive-sync-aviso\nNice=10\nIOSchedulingClass=idle\n").arg(home);
}

OldSetup makeOldSetup(const QString &root) {
    OldSetup o;
    o.home = root + "/home";
    QDir d;
    for (const char *folder : {"/.config/systemd/user", "/.local/bin", "/.config/rclone",
                               "/.local/state/rclone/bisync", "/.local/state/drive-sync", "/Drive"})
        d.mkpath(o.home + folder);
    const QString units = o.home + "/.config/systemd/user/";
    write(units + "drive-sync.service", oldServiceText(o.home).toUtf8());
    write(units + "drive-sync.timer", "[Unit]\nDescription=a cada 15 min\n\n[Timer]\nOnBootSec=5min\n"
                                      "OnUnitActiveSec=15min\n\n[Install]\nWantedBy=timers.target\n");
    write(units + "drive-sync-vigia.service", "[Service]\nType=oneshot\nExecStart=/x/drive-sync-aviso --vigia\n");
    write(units + "drive-sync-vigia.timer", "[Timer]\nOnStartupSec=10min\nOnUnitActiveSec=1h\n");
    write(o.home + "/.local/bin/drive-sync.sh", "#!/usr/bin/env bash\necho old\n");
    write(o.home + "/.local/bin/drive-sync-aviso", "#!/usr/bin/env bash\necho old\n");
    o.filters = o.home + "/.config/rclone/gdrive-filtros.txt";
    const QByteArray text = "# Gerado pelo drive-sync.sh.\n- .git/**\n";
    write(o.filters, text);
    write(o.filters + ".md5", QCryptographicHash::hash(text, QCryptographicHash::Md5).toHex());
    write(o.home + "/.local/state/rclone/bisync/gdrive_..home_Drive.path1.lst", "# bisync listing v1\n");
    write(o.home + "/.local/state/rclone/bisync/gdrive_..home_Drive.path2.lst", "# bisync listing v1\n");
    write(o.home + "/.local/state/drive-sync/aviso", "0 0 0\n");
    write(o.home + "/.local/state/drive-sync.log", "");

    const QString rclone = root + "/rclone";
    write(rclone, QString("#!/bin/sh\necho \"rclone $*\" >> '%1/calls.log'\n"
                          "[ \"$1\" = listremotes ] && echo gdrive:\nexit 0\n").arg(root).toUtf8());
    QFile::setPermissions(rclone, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    o.paths = {o.home + "/.config/systemd/user", o.home + "/.local/bin", o.home + "/.local/state/drive-sync",
               o.home + "/.config/drivesync", o.home + "/.local/share/drivesync/taken-over",
               recordingSystemctl(root), rclone};
    o.paths.appState = o.home + "/.local/state/drivesync";
    o.paths.systemUserUnits = root + "/etc-systemd-user";
    return o;
}

// The state a takeover wants: last run a success, nothing running.
Snapshot settledSnapshot() {
    Snapshot s;
    s.summary.status = "ok";
    s.lastRunOutcome = "success";
    return s;
}

// A systemctl for a takeover: it records every call, fails the failAt-th call
// (0 never fails), says the service is in serviceState and that the active
// drive-sync.service comes from fragmentDir.
QString takeoverSystemctl(const QString &root, int failAt, const QString &serviceState = "inactive",
                          const QString &fragmentDir = "/usr/lib/systemd/user", const QString &onShow = ":") {
    const QString path = root + "/systemctl";
    write(path, QString("#!/bin/sh\n"
                        "echo \"systemctl $*\" >> '%1/calls.log'\n"
                        "n=$(grep -c '^systemctl' '%1/calls.log')\n"
                        "[ \"$n\" -eq %2 ] && { echo refused >&2; exit 1; }\n"
                        "case \"$*\" in\n"
                        "  *is-active\\ drive-sync.service*) echo %3; [ %3 = active ] ;;\n"
                        "  *is-active*) echo active ;;\n"
                        "  *is-enabled*) echo enabled ;;\n"
                        "  *FragmentPath*) %5; printf 'FragmentPath=%4/drive-sync.service\\nDropInPaths=\\n\\n"
                        "FragmentPath=%4/drive-sync.timer\\nDropInPaths=\\n' ;;\n"
                        "esac\n").arg(root).arg(failAt).arg(serviceState, fragmentDir, onShow).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Every file under root with its contents, so two moments can be compared.
QMap<QString, QByteArray> filesUnder(const QString &root, const QStringList &skip = {}) {
    QMap<QString, QByteArray> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = QDir(root).relativeFilePath(path);
        if (std::any_of(skip.cbegin(), skip.cend(), [&](const QString &s) { return relative.startsWith(s); }))
            continue;
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        files.insert(relative, file.readAll());
    }
    return files;
}

// Only the systemctl calls in calls.log (detection also asks rclone).
QStringList systemctlCalls(const QString &dir) {
    QStringList found;
    for (const QString &call : calls(dir))
        if (call.startsWith("systemctl"))
            found << call;
    return found;
}

// A drivesync that appends its arguments to calls.log and answers with
// answer.json for "takeover", or fails "sync" when told to.
QString fakeDrivesync(const QString &dir, const QByteArray &takeoverAnswer, bool syncFails = false) {
    write(dir + "/answer.json", takeoverAnswer);
    const QString path = dir + "/drivesync";
    write(path, QString("#!/bin/sh\n"
                        "echo \"drivesync $*\" >> '%1/calls.log'\n"
                        "case \"$1\" in\n"
                        "  takeover) cat '%1/answer.json' ;;\n"
                        "  sync) %2 ;;\n"
                        "  *) echo '{\"ok\":true,\"error\":\"\"}' ;;\n"
                        "esac\n")
                    .arg(dir, syncFails ? "echo '{\"ok\":false,\"error\":\"denied\"}'; exit 1"
                                        : "echo '{\"ok\":true,\"error\":\"\"}'")
                    .toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

}  // namespace

class DrivesyncTests : public QObject {
    Q_OBJECT

private slots:
    void parsesASuccessfulRun()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(15, 28, 19));
        const auto runs = SyncLog::parse(successfulRun(start, 485));
        QCOMPARE(runs.size(), 1);
        const SyncRun &run = runs.first();
        QCOMPARE(run.outcome, QString("success"));
        QCOMPARE(run.start, start);
        QCOMPARE(run.start.secsTo(run.end), 485);
        QCOMPARE(run.cloud.modified, 1);
        QCOMPARE(run.notebook.added, 1);
        QCOMPARE(run.listed, 16680LL);
        QVERIFY(!run.resync);
        QCOMPARE(run.changes.size(), 2);  // The "Queue copy" line repeats a change.
        QCOMPARE(run.changes.at(0).side, QString("cloud"));
        QCOMPARE(run.changes.at(0).kind, QString("modified"));
        QCOMPARE(run.changes.at(0).path, QString("Notes/todo.md"));
        QCOMPARE(run.changes.at(1).side, QString("notebook"));
        QCOMPARE(run.changes.at(1).kind, QString("new"));
        QVERIFY(run.errors.isEmpty());
    }

    void readsTheCauseOfAFailedRun()
    {
        const QDateTime start(QDate(2026, 11, 6), QTime(20, 48, 47));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
        log += "Tip: here are the filenames we were looking for. Do they exist? \n";
        log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
        log += line(start.addSecs(1), "NOTICE", "Failed to bisync: bisync aborted");

        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);  // The trailing lines belong to the same run.
        QCOMPARE(runs.first().outcome, QString("failed"));
        QCOMPARE(runs.first().causeKind, QString("lostListings"));
        QCOMPARE(runs.first().errors.size(), 3);
    }

    void namesTheFirstRealErrorAsTheCause()
    {
        const QDateTime start(QDate(2026, 11, 6), QTime(20, 10, 0));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(30), "ERROR", "-                   Size not equal in listing. Path1: 5972, Path2: 7617 - GitHub/app/x.yml");
        log += line(start.addSecs(30), "ERROR", "Bisync aborted. Must run --resync to recover.");
        const auto outOfSync = SyncLog::parse(log);
        QCOMPARE(outOfSync.first().causeKind, QString("error"));
        QVERIFY(outOfSync.first().causeDetail.startsWith("Size not equal in listing"));

        log += line(start.addSecs(31), "ERROR", "Bisync critical error: path1 and path2 are out of sync, run --resync to recover");
        QCOMPARE(SyncLog::parse(log).first().causeKind, QString("outOfSync"));
    }

    void countsAnErrorBeforeAnyRunAsAFailedRun()
    {
        const QDateTime when(QDate(2026, 11, 7), QTime(9, 0, 0));
        QString log = successfulRun(when.addSecs(-3600), 480);
        log += line(when, "CRITICAL", "Failed to create file system for \"gdrive:\": couldn't find root directory ID");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.first().outcome, QString("success"));
        QCOMPARE(runs.last().outcome, QString("failed"));
        QCOMPARE(runs.last().causeKind, QString("error"));
        QVERIFY(runs.last().causeDetail.contains("couldn't find root directory ID"));
    }

    void marksCutOffAndOpenRuns()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(10, 0, 0));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(60), "INFO", "Path1 checking for diffs");
        log += line(start.addSecs(900), "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(960), "INFO", "Applying changes");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.first().outcome, QString("interrupted"));
        QCOMPARE(runs.last().outcome, QString("open"));
        QCOMPARE(runs.last().phase, QString("applying"));
        QCOMPARE(runs.last().lastSeen, start.addSecs(960));
    }

    void skipsARunWhoseStartWasCutOff()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(10, 0, 0));
        QString log;
        log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
        log += successfulRun(start.addSecs(600), 480);
        QCOMPARE(SyncLog::parse(log, 40, true).size(), 1);
        QCOMPARE(SyncLog::parse(log, 40, false).size(), 2);
    }

    void spotsAResyncAndCapsItsChanges()
    {
        const QDateTime start(QDate(2026, 11, 2), QTime(9, 5, 16));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "INFO", "Copying Path2 files to Path1");
        for (int i = 0; i < 305; ++i)
            log += line(start, "INFO", change("Path2", "File is new", QString("f%1.txt").arg(i)));
        log += line(start.addSecs(9000), "INFO", "Resync updating listings");
        log += line(start.addSecs(9002), "INFO", "Bisync successful");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);
        QVERIFY(runs.first().resync);
        QCOMPARE(runs.first().changes.size(), 300);
        QCOMPARE(runs.first().changesOmitted, 5);
    }

    void keepsTheNewestRuns()
    {
        QString log;
        const QDateTime start(QDate(2026, 11, 7), QTime(0, 0, 0));
        for (int i = 0; i < 5; ++i)
            log += successfulRun(start.addSecs(i * 900), 480);
        const auto runs = SyncLog::parse(log, 3);
        QCOMPARE(runs.size(), 3);
        QCOMPARE(runs.last().start, start.addSecs(4 * 900));
    }

    void readsTheTailFromAWholeLine()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("drive-sync.log");
        QVERIFY(write(path, "first line\nsecond line\nthird line\n"));
        bool found = false, truncated = false;
        QCOMPARE(SyncLog::readTail(path, 20, &found, &truncated), QString("third line\n"));
        QVERIFY(found);
        QVERIFY(truncated);
        QCOMPARE(SyncLog::readTail(path, 1000, &found, &truncated).count('\n'), 3);
        QVERIFY(!truncated);
        SyncLog::readTail(dir.filePath("missing.log"), 1000, &found);
        QVERIFY(!found);
    }

    // A log with no run (new, or just rotated) proves nothing: not green.
    void summarizesALogWithNoRuns()
    {
        SyncInputs in = healthy();
        in.lastStart = QDateTime();
        in.lastOutcome.clear();
        in.lastSuccess = QDateTime();
        QCOMPARE(Backend::summarize(in).status, QString("noRuns"));
        QCOMPARE(toneFor("noRuns"), QString("warning"));
        in.conflicts = 2;
        QCOMPARE(Backend::summarize(in).status, QString("noRuns"));
        QCOMPARE(Backend::summarize(in).warnings, QStringList{"conflicts"});
        in.driveFound = false;  // the more urgent headline wins
        QCOMPARE(Backend::summarize(in).status, QString("noDrive"));

        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources("", t0);
        src.driveFound = true;
        const Snapshot snap = buildSnapshot(src);
        QCOMPARE(snap.summary.status, QString("noRuns"));
        QCOMPARE(snap.tone, QString("warning"));
        QVERIFY(!snap.lastSuccess.isValid());
    }

    void summarizesAHealthySync()
    {
        const auto s = Backend::summarize(healthy());
        QCOMPARE(s.status, QString("ok"));
        QVERIFY(s.warnings.isEmpty());
    }

    void putsTheWorstProblemFirst()
    {
        SyncInputs in = healthy();
        in.lastOutcome = "failed";
        in.lastCauseKind = "lostListings";
        in.conflicts = 2;
        auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("failed"));
        QCOMPARE(s.causeKind, QString("lostListings"));
        QCOMPARE(s.warnings, QStringList{"conflicts"});

        in.running = true;
        QCOMPARE(Backend::summarize(in).status, QString("running"));

        in = healthy();
        in.conflicts = 1;
        in.timerFileState = "disabled";
        s = Backend::summarize(in);
        QCOMPARE(s.status, QString("conflicts"));
        QCOMPARE(s.warnings, QStringList{"timerDisabled"});

        in = healthy();
        in.serviceLoad = "not-found";
        QCOMPARE(Backend::summarize(in).status, QString("notLoaded"));
    }

    void trustsSystemdOnlyForItsNewestRun()
    {
        SyncInputs in = healthy();
        in.serviceResult = "timeout";
        auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("failed"));
        QCOMPARE(s.causeKind, QString("timeout"));

        // A resync from drive-sync.sh ran after the service failed, and worked.
        in.lastStart = in.serviceStart.addSecs(3600);
        QCOMPARE(Backend::summarize(in).status, QString("ok"));
    }

    void tellsATimerLeftOffFromAResync()
    {
        SyncInputs in = healthy();
        in.timerActive = "inactive";
        QCOMPARE(Backend::summarize(in).status, QString("timerOff"));

        in.resyncRunning = true;
        const auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("resync"));
        QVERIFY(s.warnings.isEmpty());

        in.resyncRunning = false;
        in.lastOutcome = "failed";
        QCOMPARE(Backend::summarize(in).warnings, QStringList{"timerOff"});
    }

    void noticesRunsThatStopped()
    {
        SyncInputs in = healthy();
        in.lastSuccess = in.now.addSecs(-3 * 3600);
        QCOMPARE(Backend::summarize(in).status, QString("stale"));
        in.lastOutcome = "interrupted";
        QCOMPARE(Backend::summarize(in).status, QString("interrupted"));
    }

    void parsesSystemctl()
    {
        const auto units = Backend::parseShow("LoadState=loaded\nActiveState=inactive\nExecMainStartTimestamp=@1794076098\n\n"
                                              "LoadState=loaded\nActiveState=active\n");
        QCOMPARE(units.size(), 2);
        QCOMPARE(units.at(0).value("ExecMainStartTimestamp"), QString("@1794076098"));
        QCOMPARE(units.at(1).value("ActiveState"), QString("active"));

        const QDateTime next = Backend::parseNextElapse(R"([{"next":1794076998808087,"unit":"drive-sync.timer"}])");
        QCOMPARE(next.toMSecsSinceEpoch(), 1794076998808LL);
        QVERIFY(!Backend::parseNextElapse("[]").isValid());
        QVERIFY(!Backend::parseNextElapse(R"([{"next":0}])").isValid());
    }

    void checksThePidInTheResyncMark()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("state");
        QDir(dir.path()).mkpath("proc/4242");
        QDir(dir.path()).mkpath("proc/4343");
        const QString state = dir.filePath("state"), proc = dir.filePath("proc");
        QVERIFY(!Backend::resyncRunning(state, proc));

        QVERIFY(write(dir.filePath("proc/4242/cmdline"), QByteArray("bash\0/home/someone/.local/bin/drive-sync.sh\0resync\0", 52)));
        QVERIFY(write(dir.filePath("proc/4343/cmdline"), QByteArray("vim\0notes.md\0", 13)));
        QVERIFY(write(state + "/pausado", "4242\n"));
        QVERIFY(Backend::resyncRunning(state, proc));
        QVERIFY(write(state + "/pausado", "4343\n"));
        QVERIFY(!Backend::resyncRunning(state, proc));
        QVERIFY(write(state + "/pausado", "9999\n"));
        QVERIFY(!Backend::resyncRunning(state, proc));
    }

    void watchesARealFolder()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive/Notes");
        QDir(dir.path()).mkpath("Drive/.git");
        QDir(dir.path()).mkpath("state");
        QVERIFY(write(dir.filePath("Drive/Notes/todo.md.conflict1"), "older"));
        QVERIFY(write(dir.filePath("Drive/.git/index.conflict1"), "never synced"));
        QVERIFY(write(dir.filePath("Drive/Notes/todo.md"), "newer"));
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));

        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find")};
        paths.appState = dir.filePath("app");
        Backend backend(paths);
        QTRY_COMPARE(backend.conflicts().size(), 1);
        // The scan leaves its count for the status command and the bar.
        QTRY_COMPARE(readConflictCache(paths.appState + "/conflicts.json").count, 1);
        QCOMPARE(backend.conflicts().first().toMap().value("path").toString(), QString("Notes/todo.md.conflict1"));
        QCOMPARE(backend.conflicts().first().toMap().value("folder").toString(), QString("Notes"));
        QTRY_COMPARE(backend.status(), QString("conflicts"));
        QCOMPARE(backend.runs().size(), 1);
        QCOMPARE(backend.typicalSeconds(), 480);
        QVERIFY(backend.nextRun() > QDateTime::currentDateTime());
        QVERIFY(backend.currentRun().isEmpty());

        // A run starts: the log knows before the systemd poll does.
        QFile log(paths.log);
        QVERIFY(log.open(QIODevice::Append));
        log.write(line(QDateTime::currentDateTime(), "INFO",
                       R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")").toUtf8());
        log.close();
        QTRY_COMPARE(backend.status(), QString("running"));
        QCOMPARE(backend.currentRun().value("phase").toString(), QString("listing"));
        QCOMPARE(backend.runs().size(), 1);
    }

    // The window colors itself from the same tone the bar reads.
    void backendExposesTone()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                                 QStandardPaths::findExecutable("find")};
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QCOMPARE(backend.tone(), QString("ok"));
    }

    void reportsAMissingDriveAndLog()
    {
        QTemporaryDir dir;
        BackendPaths paths{dir.filePath("missing.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), QDateTime::currentDateTime()),
                           QStandardPaths::findExecutable("find")};
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("noLog"));
        QCOMPARE(backend.conflictError(), QString("noDrive"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QVERIFY(write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Runs").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Runs": "Execuções", "Quit": "Sair"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Runs": "Rodadas"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Runs"), QString("Rodadas"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Sair"));
        QVERIFY(translator.translate("Main", "Space").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    // The shipped catalogs: valid JSON, and every plural string of the
    // Portuguese one has the forms pt-BR uses.
    void shippedCatalogsParse()
    {
        for (const QString &code : {QString("en"), QString("pt-BR")}) {
            QFile file(QString(SRCDIR) + "/../src/i18n/" + code + ".json");
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
            QJsonParseError error;
            const QJsonObject catalog = QJsonDocument::fromJson(file.readAll(), &error).object();
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(code + ": " + error.errorString()));
            QVERIFY(!catalog.isEmpty());
            for (auto it = catalog.begin(); it != catalog.end(); ++it)
                if (it.value().isObject() && code == "pt-BR")
                    QVERIFY2(it.value().toObject().contains("one") && it.value().toObject().contains("other"),
                             qPrintable(it.key()));
        }
    }

    // The tone groups every headline the way the window colors it.
    void toneGroupsEveryStatus()
    {
        for (const char *s : {"failed", "timerOff", "unavailable", "notLoaded", "noLog", "noDrive"})
            QCOMPARE(toneFor(s), QString("error"));
        for (const char *s : {"interrupted", "skipped", "stale", "noRuns", "conflicts"})
            QCOMPARE(toneFor(s), QString("warning"));
        QCOMPARE(toneFor("running"), QString("busy"));
        QCOMPARE(toneFor("resync"), QString("busy"));
        QCOMPARE(toneFor("ok"), QString("ok"));
        // A status nobody gave a tone is a mistake, and must not look fine.
        QCOMPARE(toneFor("somethingNew"), QString("error"));
        QCOMPARE(toneFor(""), QString("error"));
    }

    // Two failures after a success: the problem started with the first one.
    void snapshotFailingSinceIsFirstFailureAfterSuccess()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const QString log = successfulRun(t0, 480) + failedRun(t0.addSecs(900)) + failedRun(t0.addSecs(1800));
        const SnapshotSources src = idleSources(log, t0.addSecs(2000));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("failed"));
        QCOMPARE(s.failingSince, src.runs.at(1).start);
    }

    // The log ends in a success but systemd says the last start failed: the
    // trouble began with that start, not with the good run before it.
    void snapshotFailingSinceForAFailureOnlySystemdSaw()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources(successfulRun(t0, 480), t0.addSecs(1000));
        const QDateTime started = t0.addSecs(900).toUTC();
        src.service.insert("Result", "exit-code");
        src.service.insert("ExecMainStartTimestamp", QString("@%1").arg(started.toSecsSinceEpoch()));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("failed"));
        QCOMPARE(s.summary.causeKind, QString("systemd"));
        QCOMPARE(s.failingSince, started);
    }

    void snapshotNoFailingSinceWhenOk()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const Snapshot s = buildSnapshot(idleSources(successfulRun(t0, 480), t0.addSecs(600)));
        QCOMPARE(s.summary.status, QString("ok"));
        QCOMPARE(s.tone, QString("ok"));
        QVERIFY(!s.failingSince.isValid());
    }

    // A manual run only makes sense with the unit loaded and nothing going.
    void snapshotCanSyncNowOnlyWhenIdle()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources(successfulRun(t0, 480), t0.addSecs(600));
        QVERIFY(buildSnapshot(src).canSyncNow);

        SnapshotSources busy = src;
        busy.service["ActiveState"] = "activating";
        QVERIFY(!buildSnapshot(busy).canSyncNow);

        SnapshotSources resync = src;
        resync.resyncRunning = true;
        QVERIFY(!buildSnapshot(resync).canSyncNow);

        SnapshotSources noSystemctl = src;
        noSystemctl.systemctlWorks = false;
        QVERIFY(!buildSnapshot(noSystemctl).canSyncNow);

        SnapshotSources notLoaded = src;
        notLoaded.service["LoadState"] = "not-found";
        QVERIFY(!buildSnapshot(notLoaded).canSyncNow);
    }

    void snapshotLastRunIsNewestFinished()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const SnapshotSources src = idleSources(successfulRun(t0, 480) + failedRun(t0.addSecs(900)), t0.addSecs(1000));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.lastRunStart, src.runs.at(1).start);
        QCOMPARE(s.lastRunEnd, src.runs.at(1).end);
        QCOMPARE(s.lastRunOutcome, QString("failed"));
        QCOMPARE(s.lastSuccess, src.runs.at(0).end);
    }

    // Version 1 of what "drivesync status --json" prints: every key always
    // there, times as Unix seconds, 0 when unknown.
    void statusJsonContract()
    {
        Snapshot s;
        s.summary.status = "failed";
        s.summary.causeKind = "outOfSync";
        s.summary.warnings = {"timerDisabled"};
        s.tone = "error";
        s.lastSuccess = QDateTime(QDate(2026, 11, 7), QTime(15, 0, 0), QTimeZone::UTC);
        s.failingSince = QDateTime(QDate(2026, 11, 7), QTime(15, 30, 0), QTimeZone::UTC);
        s.conflicts = 2;
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        const QJsonObject json = snapshotToJson(s, QDateTime(), now);
        QCOMPARE(json.value("version").toInt(), 2);
        QCOMPARE(json.value("now").toInteger(), 1794067200);
        QCOMPARE(json.value("status").toString(), QString("failed"));
        QCOMPARE(json.value("tone").toString(), QString("error"));
        QCOMPARE(json.value("lastSuccess").toInteger(), 1794063600);
        QCOMPARE(json.value("failingSince").toInteger(), 1794065400);
        QCOMPARE(json.value("nextRun").toInteger(), 0);
        QCOMPARE(json.value("warnings").toArray(), QJsonArray{"timerDisabled"});
        QCOMPARE(json.value("conflicts").toInt(), 2);
        QVERIFY(json.value("lastRun").toObject().value("outcome").isString());
        QCOMPARE(json.value("lastRun").toObject().value("start").toInteger(), 0);
        QVERIFY(json.value("currentRun").toObject().value("phase").isString());
        QCOMPARE(json.value("managed").toBool(), false);
        QCOMPARE(json.value("paused").toBool(), false);
        QCOMPARE(json.value("pausedUntil").toInteger(), 0);
        QCOMPARE(json.keys().size(), 20);

        s.managed = true;
        s.paused = true;
        s.pausedUntil = QDateTime(QDate(2026, 11, 7), QTime(17, 0, 0), QTimeZone::UTC);
        const QJsonObject paused = snapshotToJson(s, QDateTime(), now);
        QCOMPARE(paused.value("managed").toBool(), true);
        QCOMPARE(paused.value("paused").toBool(), true);
        QCOMPARE(paused.value("pausedUntil").toInteger(), 1794070800);
    }

    void conflictCacheRoundTrip()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("state/conflicts.json");  // the folder does not exist yet
        ConflictCount c;
        c.count = 3;
        c.scannedAt = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QVERIFY(writeConflictCache(file, c));
        const ConflictCount back = readConflictCache(file);
        QCOMPARE(back.count, 3);
        QCOMPARE(back.error, QString());
        QCOMPARE(back.scannedAt, c.scannedAt);
        QVERIFY(conflictCacheFresh(back, c.scannedAt.addSecs(60)));
        QVERIFY(!conflictCacheFresh(back, c.scannedAt.addSecs(kConflictCacheSeconds)));
    }

    void conflictCacheCorruptIsStale()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("conflicts.json");
        QVERIFY(write(file, "not json"));
        const ConflictCount c = readConflictCache(file);
        QVERIFY(!c.scannedAt.isValid());
        QVERIFY(!conflictCacheFresh(c, QDateTime::currentDateTimeUtc()));
    }

    // A clock that moved back must not keep an old count alive for hours.
    void conflictCacheFromTheFutureIsStale()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        ConflictCount c;
        c.scannedAt = now.addSecs(600);
        QVERIFY(!conflictCacheFresh(c, now));
    }

    void statusCommandJson()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e), 0);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("version").toInt(), 2);
        QCOMPARE(json.value("status").toString(), QString("ok"));
        QVERIFY(json.value("nextRun").toInteger() > QDateTime::currentSecsSinceEpoch());
        QVERIFY(err.isEmpty());
        // The scan it had to make is left for the next call.
        QVERIFY(conflictCacheFresh(readConflictCache(paths.appState + "/conflicts.json"), QDateTime::currentDateTimeUtc()));

        // The command reads the same pause mark the window does.
        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::currentDateTimeUtc();
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        out.clear();
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e), 0);
        const QJsonObject paused = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(paused.value("status").toString(), QString("paused"));
        QCOMPARE(paused.value("paused").toBool(), true);
        QCOMPARE(paused.value("canSyncNow").toBool(), false);
    }

    // A long outage: the last success is older than the 40 runs the window
    // lists, and the state must still say when the trouble began.
    void statusKeepsTheStartOfALongOutage()
    {
        QTemporaryDir dir;
        const QDateTime t0 = QDateTime::currentDateTime().addSecs(-46 * 900 - 600);
        QString log = successfulRun(t0, 480);
        for (int i = 1; i <= 45; ++i)
            log += failedRun(t0.addSecs(i * 900));
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), log.toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), t0.addSecs(45 * 900)),
                                 QStandardPaths::findExecutable("find"), dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e), 0);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("status").toString(), QString("failed"));
        QCOMPARE(json.value("failingSince").toInteger(), t0.addSecs(900).toSecsSinceEpoch());
        QCOMPARE(json.value("lastSuccess").toInteger(), t0.addSecs(480).toSecsSinceEpoch());
    }

    void statusCommandRejectsUnknownFlag()
    {
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--jsno"}, BackendPaths{}, o, e), 1);
        QVERIFY(err.contains("usage: drivesync"));
        QVERIFY(out.isEmpty());
    }

    // A stuck systemctl must not hang the bar: the answer is "unavailable".
    void statusCommandSystemctlTimeout()
    {
        QTemporaryDir dir;
        const QString systemctl = dir.filePath("systemctl");
        QVERIFY(write(systemctl, "#!/bin/sh\nsleep 5\n"));
        QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), systemctl, QStandardPaths::findExecutable("find"),
                                 dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e, 200), 0);
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("status").toString(), QString("unavailable"));
    }

    void pluginSetupLinksWhenAbsent()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        const QFileInfo link(paths.userPlugins + "/drivesync");
        QVERIFY(link.isSymLink());
        QCOMPARE(link.symLinkTarget(), QFileInfo(paths.shipped).absoluteFilePath());
        QVERIFY(calls(dir.path()).contains("shell-cmd shell rescanPlugins"));
    }

    // A folder the user made with the same name is theirs: never touched.
    void pluginSetupLeavesForeignFolder()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        QDir(paths.userPlugins).mkpath("drivesync");
        QVERIFY(write(paths.userPlugins + "/drivesync/mine.txt", "keep"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
        QFile mine(paths.userPlugins + "/drivesync/mine.txt");
        QVERIFY(mine.open(QIODevice::ReadOnly));
        QCOMPARE(mine.readAll(), QByteArray("keep"));
        QVERIFY(calls(dir.path()).isEmpty());
    }

    // A link someone else made (a dev copy, say) is theirs too, and so is a
    // dangling one: exists() says false for it, isSymLink() does not.
    void pluginSetupLeavesForeignAndDanglingLinks()
    {
        for (const bool dangling : {false, true}) {
            QTemporaryDir dir;
            const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
            const QString elsewhere = dir.filePath(dangling ? "gone" : "dev-copy");
            if (!dangling)
                QVERIFY(QDir().mkpath(elsewhere));
            QVERIFY(QDir().mkpath(paths.userPlugins));
            const QString link = paths.userPlugins + "/drivesync";
            QVERIFY(QFile::link(elsewhere, link));
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("blocked"));
            QCOMPARE(setup.detail(), link);
            QVERIFY(QFileInfo(link).isSymLink());
            QCOMPARE(QFileInfo(link).symLinkTarget(), elsewhere);
            QVERIFY(calls(dir.path()).isEmpty());
        }
    }

    // SyncWatch's widget, renamed: the new one takes its place on the bar by
    // itself, then the old entry and its dangling link go.
    void pluginSetupTakesOverTheSyncWatchWidget()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.legacyShipped = dir.filePath("gone/omarchy-plugin");  // pacman removed it
        const QString omarchy = QString(
            "#!/bin/sh\n"
            "echo \"$*\" >> '%1/calls.log'\n"
            "case \"$*\" in\n"
            "  'plugin list --json') cat '%1/list.json' ;;\n"
            "  'plugin enable drivesync --after syncwatch') echo '[{\"id\":\"drivesync\",\"enabled\":true}]' > '%1/list.json' ;;\n"
            "esac\n").arg(dir.path());
        QVERIFY(write(paths.omarchy, omarchy.toUtf8()));
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(paths.legacyShipped, paths.userPlugins + "/syncwatch"));

        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("enabled"));
        QCOMPARE(calls(dir.path()), (QStringList{"shell-cmd shell rescanPlugins", "plugin list --json",
                                                 "plugin enable drivesync --after syncwatch",
                                                 "plugin disable syncwatch", "plugin list --json"}));
        QVERIFY(!QFileInfo(paths.userPlugins + "/syncwatch").isSymLink());
        QVERIFY(QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
    }

    // The old link was there but its widget was not on the bar: the enable
    // finds no place, nothing is replaced, and the usual offer follows.
    void pluginSetupSyncWatchWidgetOffBarFallsBackToTheOffer()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.legacyShipped = dir.filePath("gone/omarchy-plugin");
        const QString omarchy = QString(
            "#!/bin/sh\n"
            "echo \"$*\" >> '%1/calls.log'\n"
            "case \"$*\" in\n"
            "  'plugin list --json') cat '%1/list.json' ;;\n"
            "  'plugin enable'*) echo 'could not find target widget syncwatch' >&2; exit 1 ;;\n"
            "esac\n").arg(dir.path());
        QVERIFY(write(paths.omarchy, omarchy.toUtf8()));
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(paths.legacyShipped, paths.userPlugins + "/syncwatch"));

        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QVERIFY(!calls(dir.path()).contains("plugin disable syncwatch"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/syncwatch").isSymLink());
    }

    // A syncwatch link that points anywhere else is not SyncWatch's: never touched.
    void pluginSetupLeavesAForeignSyncWatchLink()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.legacyShipped = dir.filePath("gone/omarchy-plugin");
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(dir.filePath("mine"), paths.userPlugins + "/syncwatch"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QVERIFY(QFileInfo(paths.userPlugins + "/syncwatch").isSymLink());
        QVERIFY(!calls(dir.path()).contains("plugin enable drivesync --after syncwatch"));
    }
    void pluginSetupEnabled()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), R"([{"id":"drivesync","enabled":true}])");
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("enabled"));
    }

    // "No" is remembered: no offer, and no link that would undo a removal.
    void pluginSetupRemembersNo()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.declined = dir.filePath("app/bar-declined");
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("offer"));
            setup.decline();
            QCOMPARE(setup.state(), QString("declined"));
        }
        QFile::remove(paths.userPlugins + "/drivesync");  // as `omarchy plugin remove` would
        QFile::remove(dir.filePath("calls.log"));
        PluginSetup again(paths);
        again.check();
        QCOMPARE(again.state(), QString("declined"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
        QVERIFY(calls(dir.path()).isEmpty());
    }

    // omarchy failing to list its plugins at start is no banner: nobody asked
    // for anything. After B the same failure is.
    void pluginSetupListFailureNeedsAnAsk()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        const QString omarchy = QString(
            "#!/bin/sh\n"
            "echo \"$*\" >> '%1/calls.log'\n"
            "case \"$*\" in\n"
            "  'plugin list --json') [ -e '%1/list-fails' ] && { echo 'no shell' >&2; exit 1; }; cat '%1/list.json' ;;\n"
            "esac\n").arg(dir.path());
        QVERIFY(write(paths.omarchy, omarchy.toUtf8()));
        QVERIFY(write(dir.filePath("list-fails"), ""));
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("unchecked"));
            QCOMPARE(setup.detail(), QString("no shell"));
        }
        QFile::remove(dir.filePath("list-fails"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QVERIFY(write(dir.filePath("list-fails"), ""));
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("failed"));
        QCOMPARE(setup.detail(), QString("no shell"));
    }

    // The notice that the path is taken can be hidden for good, like the offer.
    void pluginSetupBlockedCanBeDismissed()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.declined = dir.filePath("app/bar-declined");
        QVERIFY(QDir(paths.userPlugins).mkpath("drivesync"));
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("blocked"));
            setup.decline();
            QCOMPARE(setup.state(), QString("declined"));
            QVERIFY(QFileInfo::exists(paths.declined));
        }
        PluginSetup again(paths);
        again.check();
        QCOMPARE(again.state(), QString("declined"));
        QVERIFY(QFileInfo(paths.userPlugins + "/drivesync").isDir());  // still theirs
        QVERIFY(calls(dir.path()).isEmpty());
    }

    void pluginSetupWithoutOmarchy()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.omarchy.clear();
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("noOmarchy"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
    }
    void syncConfigRoundTrips()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("drivesync/config.json");
        const SyncConfig config = oldUnitConfig();
        QString error;
        QVERIFY2(saveSyncConfig(file, config, &error), qPrintable(error));
        const auto loaded = loadSyncConfig(file, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(loaded->remote, config.remote);
        QCOMPARE(loaded->localPath, config.localPath);
        QCOMPARE(loaded->workDir, config.workDir);
        QCOMPARE(loaded->filtersFile, config.filtersFile);
        QCOMPARE(loaded->logFile, config.logFile);
        QCOMPARE(loaded->rcloneFlags, config.rcloneFlags);
        QCOMPARE(loaded->runOnlyWhen, config.runOnlyWhen);

        // Anything the app cannot trust is no config at all, with a reason.
        QVERIFY(write(dir.filePath("corrupt.json"), "{not json"));
        QVERIFY(!loadSyncConfig(dir.filePath("corrupt.json"), &error).has_value());
        QVERIFY(!error.isEmpty());
        QVERIFY(write(dir.filePath("v9.json"), R"({"version":9,"remote":"gdrive:"})"));
        QVERIFY(!loadSyncConfig(dir.filePath("v9.json"), &error).has_value());
        QVERIFY(write(dir.filePath("nolocal.json"),
                      R"({"version":1,"remote":"gdrive:","workDir":"/w","filtersFile":"/f","logFile":"/l","rcloneFlags":[]})"));
        QVERIFY(!loadSyncConfig(dir.filePath("nolocal.json"), &error).has_value());
        QVERIFY(error.contains("localPath"));
        QVERIFY(!loadSyncConfig(dir.filePath("missing.json"), &error).has_value());
    }

    void rcloneArgumentsMatchTheOldUnit()
    {
        const SyncConfig config = oldUnitConfig();
        const QStringList expected{"bisync", "gdrive:", "/home/someone/Drive",
                                   "--workdir", "/home/someone/.local/state/rclone/bisync",
                                   "--filters-file", "/home/someone/.config/drivesync/filters.txt",
                                   "--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                                   "--max-lock", "2m", "--max-delete", "5", "--conflict-resolve", "newer",
                                   "--log-level", "INFO", "--fast-list",
                                   "--log-file", "/home/someone/.local/state/drive-sync.log"};
        const QStringList arguments = rcloneRunArguments(config);
        QCOMPARE(arguments, expected);

        // The same tokens the real unit passes, filters path aside, plus
        // --fast-list: no other flag invented or dropped.
        QStringList real = QProcess::splitCommand(QString(kOldExecStart)).mid(1);
        real.replace(real.indexOf("/home/someone/.config/rclone/gdrive-filtros.txt"),
                     "/home/someone/.config/drivesync/filters.txt");
        real << "--fast-list";
        QStringList a = arguments, b = real;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        QCOMPARE(a, b);
    }

    void rcloneArgumentsAddFastListOnce()
    {
        SyncConfig config = oldUnitConfig();
        config.rcloneFlags << "--fast-list";
        QCOMPARE(rcloneRunArguments(config).count("--fast-list"), 1);

        // Turning ListR off on purpose wins over the default.
        config = oldUnitConfig();
        config.rcloneFlags << "--disable" << "ListR";
        QVERIFY(!rcloneRunArguments(config).contains("--fast-list"));
    }

    void migrationMovesSyncWatchFoldersKeepingEveryByte()
    {
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        const QByteArray md5 = readBytes(dir.path() + "/.config/syncwatch/filters.txt.md5");

        const QStringList problems = migrateFromSyncWatch(dir.path());
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
        for (const LegacyFolder &folder : legacyFolders(dir.path())) {
            QVERIFY2(!QFileInfo::exists(folder.from), qPrintable(folder.from));
            QVERIFY2(QFileInfo(folder.to).isDir(), qPrintable(folder.to));
        }
        const QString config = dir.path() + "/.config/drivesync";
        QCOMPARE(readBytes(config + "/filters.txt"), legacy.filters);
        QCOMPARE(readBytes(config + "/filters.txt.md5"), md5);
        QVERIFY(QFile::exists(dir.path() + "/.local/state/drivesync/paused.json"));

        // The config names its filters at the new place, and a run would start.
        QString error;
        const auto sync = loadSyncConfig(config + "/config.json", &error);
        QVERIFY2(sync.has_value(), qPrintable(error));
        QCOMPARE(sync->filtersFile, config + "/filters.txt");
        QCOMPARE(sync->logFile, dir.path() + "/.local/state/drive-sync.log");
        QVERIFY2(filtersUsable(sync->filtersFile, &error), qPrintable(error));

        // The manifest points an undo at the moved backup; where the file came from does not change.
        const QString backup = dir.path() + "/.local/share/drivesync/taken-over/20261110-224919";
        const QJsonObject move = QJsonDocument::fromJson(readBytes(backup + "/manifest.json"))
                                     .object().value("moved").toArray().first().toObject();
        QCOMPARE(move.value("to").toString(), backup + "/bin/drive-sync.sh");
        QCOMPARE(move.value("from").toString(), dir.path() + "/.local/bin/drive-sync.sh");
        QVERIFY(QFile::exists(move.value("to").toString()));
    }
    void migrationRunTwiceChangesNothing()
    {
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        QVERIFY(migrateFromSyncWatch(dir.path()).isEmpty());
        const QString configFile = dir.path() + "/.config/drivesync/config.json";
        const QByteArray once = readBytes(configFile);
        const QDateTime written = QFileInfo(configFile).lastModified();

        QVERIFY(migrateFromSyncWatch(dir.path()).isEmpty());
        QCOMPARE(readBytes(configFile), once);
        QCOMPARE(QFileInfo(configFile).lastModified(), written);

        // A home that never had SyncWatch gets no folder at all.
        QTemporaryDir fresh;
        QVERIFY(migrateFromSyncWatch(fresh.path()).isEmpty());
        QVERIFY(QDir(fresh.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void migrationMergesWhenBothFoldersExist()
    {
        // An old SyncWatch window still open writes its conflict cache to the
        // old state folder after the new one exists.
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        const QString oldState = dir.path() + "/.local/state/syncwatch";
        const QString newState = dir.path() + "/.local/state/drivesync";
        QVERIFY(QDir().mkpath(newState));
        QVERIFY(write(newState + "/conflicts.json", "new"));
        QVERIFY(write(oldState + "/conflicts.json", "old"));

        const QStringList problems = migrateFromSyncWatch(dir.path());
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems.first().contains(oldState + "/conflicts.json"));
        // What the new folder lacked moved; what both had kept the new copy.
        QVERIFY(QFile::exists(newState + "/paused.json"));
        QVERIFY(!QFile::exists(oldState + "/paused.json"));
        QCOMPARE(readBytes(newState + "/conflicts.json"), QByteArray("new"));
        QCOMPARE(readBytes(oldState + "/conflicts.json"), QByteArray("old"));
        // The other folders had no new copy and moved whole.
        QVERIFY(!QFileInfo::exists(dir.path() + "/.config/syncwatch"));
        QVERIFY(QFile::exists(dir.path() + "/.config/drivesync/filters.txt.md5"));

        // Once the old copy is gone, the next start empties and removes the old folder.
        QVERIFY(QFile::remove(oldState + "/conflicts.json"));
        QVERIFY(migrateFromSyncWatch(dir.path()).isEmpty());
        QVERIFY(!QFileInfo::exists(oldState));
    }
    void halfMigratedConfigRefusesToRun()
    {
        // Folders moved but the config not rewritten (a crash in between):
        // the config still names the old filters, so no run starts until the
        // next command finishes the job.
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        QVERIFY(moveLegacyFolders(legacyFolders(dir.path())).isEmpty());
        QString error;
        const auto sync = loadSyncConfig(dir.path() + "/.config/drivesync/config.json", &error);
        QVERIFY2(sync.has_value(), qPrintable(error));
        QCOMPARE(sync->filtersFile, dir.path() + "/.config/syncwatch/filters.txt");
        QString refusal;
        QVERIFY(runCommandPlan(*sync, &refusal).isEmpty());
        QVERIFY(!refusal.isEmpty());

        QVERIFY(migrateFromSyncWatch(dir.path()).isEmpty());
        const auto fixed = loadSyncConfig(dir.path() + "/.config/drivesync/config.json", &error);
        QVERIFY(!runCommandPlan(*fixed, &refusal).isEmpty());
    }
    void filtersUsableRequiresMatchingMd5()
    {
        QTemporaryDir dir;
        const QString filters = dir.filePath("filters.txt");
        QString why;
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY(why.contains("filters.txt"));

        const QByteArray text = "- .git/**\n- node_modules/**\n";
        QVERIFY(write(filters, text));
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY(why.contains(".md5"));

        // bisync writes the hex digest alone, with no newline.
        QVERIFY(write(filters + ".md5", QCryptographicHash::hash(text, QCryptographicHash::Md5).toHex()));
        QVERIFY2(filtersUsable(filters, &why), qPrintable(why));

        QVERIFY(write(filters, text + "- .venv/**\n"));
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY(why.contains("changed"));
    }
    void pauseMarkRoundTrips()
    {
        QTemporaryDir dir;
        const QString file = pauseFilePath(dir.filePath("state"));  // the folder does not exist yet
        QCOMPARE(file, dir.filePath("state") + "/paused.json");
        QVERIFY(!readPause(file).present);

        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        mark.until = QDateTime::fromSecsSinceEpoch(1794070800, QTimeZone::UTC);
        QVERIFY(writePause(file, mark));
        PauseMark back = readPause(file);
        QVERIFY(back.present);
        QCOMPARE(back.since, mark.since);
        QCOMPARE(back.until, mark.until);
        QVERIFY(!back.lastReminder.isValid());

        mark.until = QDateTime();  // until the user resumes
        mark.lastReminder = mark.since.addSecs(86400);
        QVERIFY(writePause(file, mark));
        back = readPause(file);
        QVERIFY(!back.until.isValid());
        QCOMPARE(back.lastReminder, mark.lastReminder);

        QVERIFY(clearPause(file));
        QVERIFY(!readPause(file).present);
        QVERIFY(clearPause(file));  // nothing to clear is fine
    }

    void corruptPauseFailsSafe()
    {
        // A mark that cannot be read still holds the runs: syncing while the
        // user asked for a pause is worse than one extra "paused".
        QTemporaryDir dir;
        const QString file = dir.filePath("paused.json");
        QVERIFY(write(file, "{oops"));
        const PauseMark mark = readPause(file);
        QVERIFY(mark.present);
        QVERIFY(!mark.until.isValid());
        QVERIFY(!pauseEnded(mark, QDateTime::currentDateTimeUtc()));
    }

    void nextMorningPicksTheNext8am()
    {
        const QTimeZone zone("America/Sao_Paulo");
        const QDate day(2026, 11, 9);
        auto at = [&](const QDate &d, int h, int m) { return QDateTime(d, QTime(h, m), zone); };
        QCOMPARE(nextMorning(at(day, 7, 59)), at(day, 8, 0).toUTC());
        QCOMPARE(nextMorning(at(day, 8, 0)), at(day.addDays(1), 8, 0).toUTC());
        QCOMPARE(nextMorning(at(day, 23, 0)), at(day.addDays(1), 8, 0).toUTC());
        QCOMPARE(nextMorning(at(day, 8, 0)).timeSpec(), Qt::UTC);
    }

    void pauseEndedOnlyAfterItsEnd()
    {
        PauseMark mark;
        mark.present = true;
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QVERIFY(!pauseEnded(mark, now));  // no end
        mark.until = now.addSecs(60);
        QVERIFY(!pauseEnded(mark, now));
        mark.until = now;
        QVERIFY(pauseEnded(mark, now));
        QVERIFY(!pauseEnded(PauseMark(), now));  // no mark, nothing ended
    }

    void pausedBeatsLateAndFailed()
    {
        SyncInputs in = healthy();
        in.lastOutcome = "failed";
        in.lastCauseKind = "outOfSync";
        in.lastSuccess = in.now.addSecs(-3 * 3600);
        in.paused = true;
        SyncSummary s = Backend::summarize(in);
        QCOMPARE(s.status, QString("paused"));
        QVERIFY(!s.warnings.contains("stale"));
        QCOMPARE(toneFor("paused"), QString("warning"));

        in.running = true;  // a run that started before the pause still shows
        QCOMPARE(Backend::summarize(in).status, QString("running"));
    }

    void pauseEndedKeepsPausedUntilARunStarts()
    {
        // The mark stays until run-condition clears it at the next tick, so
        // between the end and that tick the state is still "paused", never
        // "skipped" from the last tick the pause held back.
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.toLocalTime().addSecs(-3600), 480), now);
        src.service["Result"] = "exec-condition";
        src.managed = true;
        src.pause.present = true;
        src.pause.since = now.addSecs(-7200);
        src.pause.until = now.addSecs(-60);
        const Snapshot snap = buildSnapshot(src);
        QCOMPARE(snap.summary.status, QString("paused"));
        QVERIFY(snap.paused);
        QVERIFY(snap.managed);
        QCOMPARE(snap.pausedUntil, src.pause.until);
        QVERIFY(snap.canSyncNow);  // the run clears the ended pause itself

        src.pause = PauseMark();
        const Snapshot after = buildSnapshot(src);
        QCOMPARE(after.summary.status, QString("skipped"));
        QVERIFY(!after.paused);
        QVERIFY(after.canSyncNow);
    }
    void syncStartsTheServiceWithoutBlocking()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, paths, o, e), 0);
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync.service"});

        out.clear();
        QCOMPARE(runSyncCommand({"--json"}, paths, o, e), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("ok").toBool(), true);

        // systemctl's own words reach the caller.
        QTemporaryDir failing;
        out.clear();
        QCOMPARE(runSyncCommand({"--json"}, controlPaths(failing.path(), false, 1), o, e), 1);
        const QJsonObject answer = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(answer.value("ok").toBool(), false);
        QVERIFY(answer.value("error").toString().contains("denied"));
        QCOMPARE(runSyncCommand({"--bogus"}, paths, o, e), 1);
    }

    void syncIsRefusedWhilePaused()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        PauseMark mark;
        mark.present = true;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, paths, o, e), 1);
        QVERIFY(calls(dir.path()).isEmpty());
        QVERIFY(err.contains("paused"));
    }

    void pauseIsRefusedWhenNotManaged()
    {
        // The old units never look at the mark, so a pause there would be a lie.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runPauseCommand({}, paths, QDateTime::currentDateTimeUtc(), o, e), 1);
        QVERIFY(!readPause(pauseFilePath(paths.appState)).present);
        QVERIFY(err.contains("DriveSync"));
    }

    void pauseWritesTheEnd()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QString out, err;
        QTextStream o(&out), e(&err);
        const QString file = pauseFilePath(paths.appState);

        QCOMPARE(runPauseCommand({"--minutes", "60"}, paths, now, o, e), 0);
        PauseMark mark = readPause(file);
        QVERIFY(mark.present);
        QCOMPARE(mark.since, now);
        QCOMPARE(mark.until, now.addSecs(3600));

        QCOMPARE(runPauseCommand({}, paths, now.addSecs(60), o, e), 0);
        mark = readPause(file);
        QVERIFY(!mark.until.isValid());
        QCOMPARE(mark.since, now);  // a longer pause keeps when the pause began

        QCOMPARE(runPauseCommand({"--until-tomorrow"}, paths, now, o, e), 0);
        QCOMPARE(readPause(file).until, nextMorning(now.toLocalTime()));

        for (const QStringList &bad : {QStringList{"--minutes"}, QStringList{"--minutes", "0"},
                                       QStringList{"--minutes", "x"}, QStringList{"--later"}})
            QCOMPARE(runPauseCommand(bad, paths, now, o, e), 1);
        QVERIFY(calls(dir.path()).isEmpty());  // pausing never touches systemd
    }

    void resumeClearsTheMarkAndStartsARun()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        PauseMark mark;
        mark.present = true;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runResumeCommand({}, paths, o, e), 0);
        QVERIFY(!readPause(pauseFilePath(paths.appState)).present);
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync.service"});
    }

    void conditionFollowsThePause()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QString file = pauseFilePath(paths.appState);
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);

        QCOMPARE(runConditionCommand(paths, config, now), 0);

        PauseMark mark;
        mark.present = true;
        mark.until = now.addSecs(60);
        QVERIFY(writePause(file, mark));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(readPause(file).present);

        // The first tick after the end runs, and the mark is gone with it.
        QCOMPARE(runConditionCommand(paths, config, now.addSecs(60)), 0);
        QVERIFY(!readPause(file).present);
    }

    void conditionSkipsWhenRunOnlyWhenFails()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        SyncConfig config = oldUnitConfig();
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        config.runOnlyWhen = "true";
        QCOMPARE(runConditionCommand(paths, config, now), 0);
        config.runOnlyWhen = "false";
        QCOMPARE(runConditionCommand(paths, config, now), 1);

        // An ended pause is not cleared by a tick that could not run anyway.
        PauseMark mark;
        mark.present = true;
        mark.until = now.addSecs(-60);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(readPause(pauseFilePath(paths.appState)).present);
    }

    void runRefusesWithoutMatchingMd5()
    {
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();
        config.filtersFile = dir.filePath("filters.txt");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        QString refusal;
        QVERIFY(runCommandPlan(config, &refusal).isEmpty());
        QVERIFY(refusal.contains("filters.txt.md5"));

        QVERIFY(write(config.filtersFile + ".md5",
                      QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        refusal.clear();
        QCOMPARE(runCommandPlan(config, &refusal), rcloneRunArguments(config));
        QVERIFY(refusal.isEmpty());
    }
    void notifyIsQuietOnExecCondition()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QCOMPARE(runNotifyCommand("exec-condition", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).isEmpty());
    }

    void notifyWarnsOnceThenHourly()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::currentDateTimeUtc();

        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY(sent.at(0).contains("-u critical"));
        QVERIFY(sent.at(0).contains("Drive stopped syncing"));

        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);  // ten minutes later: nothing new

        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(3660)), 0);
        sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY(sent.at(1).contains("Drive is still stopped"));
        // The old card goes away first, by id and by title.
        const QStringList all = calls(dir.path());
        QVERIFY(all.contains("gdbus call --session --dest org.freedesktop.Notifications --object-path "
                             "/org/freedesktop/Notifications --method org.freedesktop.Notifications.CloseNotification 41"));
        QVERIFY(all.contains("omarchy notification dismiss Drive stopped syncing"));
    }

    void notifyNamesTheCause()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).at(0).contains("bisync lost its listings"));
        QVERIFY(notifications(dir.path()).at(0).contains("drivesync takeover --undo"));

        QTemporaryDir other;
        const BackendPaths timeoutPaths = controlPaths(other.path(), true);
        QCOMPARE(runNotifyCommand("timeout", timeoutPaths, fakeNotifier(other.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(other.path()).at(0).contains("time limit"));

        // A run DriveSync refused to start leaves its reason in the log.
        QTemporaryDir refused;
        const BackendPaths refusedPaths = controlPaths(refused.path(), true);
        QVERIFY(write(refusedPaths.log, line(QDateTime::currentDateTime(), "CRITICAL",
                                             "DriveSync did not start rclone: filters.txt.md5 is missing").toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", refusedPaths, fakeNotifier(refused.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(refused.path()).at(0).contains("filters.txt.md5 is missing"));
    }

    // The card's time shows the hours of the bar clock, not the language's own.
    void notifyTimeFollowsTheBarClock()
    {
        for (const QString &format : {QString(), QString("HH:mm")}) {
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            NotifierPaths tools = fakeNotifier(dir.path());
            tools.locale = QLocale(QLocale::English, QLocale::UnitedStates);
            tools.timeFormat = format;
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, QDateTime::currentDateTimeUtc()), 0);
            // The time is on the card's second line, so the whole log is read.
            QFile log(dir.filePath("calls.log"));
            QVERIFY(log.open(QIODevice::ReadOnly));
            const QString sent = QString::fromUtf8(log.readAll());
            QVERIFY(sent.contains("Drive stopped syncing"));
            static const QRegularExpression amPm("\\d:\\d\\d.?[AP]M");
            QCOMPARE(sent.contains(amPm), format.isEmpty());
        }
    }

    void clockHoursFromAFormat()
    {
        QVERIFY(hoursInFormat("ddd d MMM HH:mm") == ClockHours::TwentyFour);
        QVERIFY(hoursInFormat("dddd h:mm:ss ap") == ClockHours::Twelve);
        QVERIFY(hoursInFormat("h:mm AP") == ClockHours::Twelve);
        QVERIFY(hoursInFormat("h:mm") == ClockHours::TwentyFour);  // Qt's h is 0 to 23 without AM/PM
        // Quoted text is literal: the "a" of "at" is no AM/PM marker.
        QVERIFY(hoursInFormat("'at' HH:mm") == ClockHours::TwentyFour);
        QVERIFY(hoursInFormat("'It''s' h:mm a") == ClockHours::Twelve);
        QVERIFY(hoursInFormat("d MMMM 'W'ww yyyy") == ClockHours::Unknown);
    }

    void clockHoursFromTheBar()
    {
        const QByteArray top = R"({"bar":{"position":"top","layout":{"center":[
            {"id":"omarchy.clock","format":"ddd d MMM h:mm AP","verticalFormat":"HH\nmm"}]}}})";
        QVERIFY(barClockHours({top}) == ClockHours::Twelve);
        // A side bar shows the vertical format.
        QByteArray side = top;
        side.replace("\"top\"", "\"left\"");
        QVERIFY(barClockHours({side}) == ClockHours::TwentyFour);
        // An entry without a format shows Omarchy's default, 24 hours.
        QVERIFY(barClockHours({R"({"bar":{"layout":{"right":[{"id":"omarchy.clock"}]}}})"}) == ClockHours::TwentyFour);
        // No clock on the bar, or nothing readable: the bar says nothing.
        QVERIFY(barClockHours({R"({"bar":{"layout":{"left":[{"id":"omarchy.workspaces"}]}}})"}) == ClockHours::Unknown);
        QVERIFY(barClockHours({"not json"}) == ClockHours::Unknown);
        // A user file without a layout leaves it to Omarchy's defaults, and
        // its own position still counts.
        const QByteArray defaults = R"({"bar":{"position":"top","layout":{"center":[
            {"id":"omarchy.clock","format":"h:mm AP","verticalFormat":"H mm"}]}}})";
        QVERIFY(barClockHours({R"({"idle":{}})", defaults}) == ClockHours::Twelve);
        QVERIFY(barClockHours({R"({"bar":{"position":"right"}})", defaults}) == ClockHours::TwentyFour);
    }

    void clockTimeFormatFallsBackToTheLocale()
    {
        const QLocale english(QLocale::English, QLocale::UnitedStates);
        const QLocale portuguese(QLocale::Portuguese, QLocale::Brazil);
        QCOMPARE(clockTimeFormat(ClockHours::TwentyFour, english), QString("HH:mm"));
        QCOMPARE(clockTimeFormat(ClockHours::Twelve, portuguese), QString("h:mm AP"));
        QCOMPARE(clockTimeFormat(ClockHours::Unknown, portuguese), QString("HH:mm"));
        QVERIFY(hoursInFormat(clockTimeFormat(ClockHours::Unknown, english)) == ClockHours::Twelve);
    }

    // A right click on the bar clock rewrites shell.json; the window follows.
    void clockFormatFollowsShellJson()
    {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("omarchy")));
        const QString user = dir.filePath("omarchy/shell.json");
        QVERIFY(write(user, R"({"bar":{"layout":{"center":[{"id":"omarchy.clock","format":"HH:mm"}]}}})"));
        ClockFormat clock(ClockPaths{{user, dir.filePath("missing-defaults.json")}, QLocale::c()});
        QCOMPARE(clock.timeFormat(), QString("HH:mm"));

        QSignalSpy changed(&clock, &ClockFormat::changed);
        // The shell saves by replacing the file.
        const QString saved = dir.filePath("omarchy/shell.json.tmp");
        QVERIFY(write(saved, R"({"bar":{"layout":{"center":[{"id":"omarchy.clock","format":"h:mm AP"}]}}})"));
        QVERIFY(QFile::remove(user));
        QVERIFY(QFile::rename(saved, user));
        QTRY_COMPARE(clock.timeFormat(), QString("h:mm AP"));
        QCOMPARE(changed.count(), 1);
    }

    void notifyWithoutASessionRetries()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        QVERIFY(write(dir.filePath("ids.txt"), "none\n"));
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QVERIFY(write(dir.filePath("ids.txt"), "7\n"));
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(60)), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        // Still the first notice: none ever reached the screen.
        QVERIFY(sent.at(1).contains("Drive stopped syncing"));
    }

    void successClosesAndSaysSinceWhen()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(900)), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY(sent.at(1).contains("-u normal"));
        QVERIFY(sent.at(1).contains("Drive is syncing again"));
        QVERIFY(calls(dir.path()).contains("omarchy notification dismiss Drive is still stopped"));

        // Once back, another success says nothing.
        QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(1800)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
    }

    void watchdogWarnsWhenRunsCannotHappen()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const struct {
            QString load, active, enabled, expected;
        } cases[] = {{"not-found", "active", "enabled", "does not load"},
                     {"loaded", "inactive", "enabled", "drive-sync.timer is inactive"},
                     {"loaded", "active", "disabled", "next boot"}};
        for (const auto &c : cases) {
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            paths.systemctl = watchdogSystemctl(dir.path(), c.load, c.active, c.enabled);
            QCOMPARE(runWatchdogCommand(paths, config, fakeNotifier(dir.path()), now), 0);
            const QStringList sent = notifications(dir.path());
            QCOMPARE(sent.size(), 1);
            QVERIFY2(sent.at(0).contains(c.expected), qPrintable(sent.at(0)));
            QVERIFY(sent.at(0).contains("Drive will not sync"));
        }

        // All well: silent, and a problem seen before is closed with a notice.
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
        QCOMPARE(runWatchdogCommand(paths, config, tools, now), 0);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(3600)), 0);
        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(7200)), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY(sent.at(1).contains("Drive timer is back"));
    }

    void watchdogRespectsRunOnlyWhen()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen = "false";
        QCOMPARE(runWatchdogCommand(paths, config, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).isEmpty());
    }

    void watchdogRemindsDailyOnlyForAPauseWithNoEnd()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        PauseMark mark;
        mark.present = true;
        mark.since = now;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));

        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(3600)), 0);
        QVERIFY(notifications(dir.path()).isEmpty());  // a fresh pause is fine
        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(86400)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QVERIFY(notifications(dir.path()).at(0).contains("DriveSync is still paused"));
        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(86400 + 3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);  // once a day
        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(2 * 86400)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);

        // A pause with an end ends by itself, so it needs no reminder.
        mark.until = now.addSecs(10 * 86400);
        mark.lastReminder = QDateTime();
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QCOMPARE(runWatchdogCommand(paths, config, tools, now.addSecs(5 * 86400)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
    }
    void detectReadsTheRealUnit()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.path());
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QCOMPARE(plan.state, QString("found"));
        QVERIFY2(plan.blockers.isEmpty(), qPrintable(plan.blockers.join(", ")));
        QVERIFY(plan.canApply());
        QCOMPARE(plan.config.remote, QString("gdrive:"));
        QCOMPARE(plan.config.localPath, old.home + "/Drive");
        QCOMPARE(plan.config.workDir, old.home + "/.local/state/rclone/bisync");
        QCOMPARE(plan.config.filtersFile, old.home + "/.config/drivesync/filters.txt");
        QCOMPARE(plan.config.logFile, old.home + "/.local/state/drive-sync.log");
        QCOMPARE(plan.config.rcloneFlags,
                 (QStringList{"--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                              "--max-lock", "2m", "--max-delete", "5", "--conflict-resolve", "newer",
                              "--log-level", "INFO"}));
        QCOMPARE(plan.config.runOnlyWhen, QString("findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'"));
        QCOMPARE(plan.oldFiltersFile, old.filters);
        for (const QString &name : {QString("drive-sync.service"), QString("drive-sync-vigia.timer"),
                                    QString("drive-sync.sh"), QString("gdrive-filtros.txt.md5")})
            QVERIFY2(plan.found.join('\n').contains(name), qPrintable(name));
    }

    void detectIsNoneWithoutUnits()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.path());
        QVERIFY(QFile::remove(old.paths.userUnits + "/drive-sync.service"));
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).state, QString("none"));
    }

    void detectIsManagedWithAConfig()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.path());
        QVERIFY(saveSyncConfig(old.paths.appConfigDir + "/config.json", oldUnitConfig(), nullptr));
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QCOMPARE(plan.state, QString("managed"));
        QVERIFY(!plan.canApply());
    }

    void detectNamesEveryBlocker()
    {
        const auto blockersAfter = [](const std::function<void(OldSetup &, Snapshot &)> &change) {
            QTemporaryDir dir;
            OldSetup old = makeOldSetup(dir.path());
            Snapshot snapshot = settledSnapshot();
            change(old, snapshot);
            const TakeoverPlan plan = detectTakeover(old.paths, snapshot);
            return plan.blockers;
        };
        QCOMPARE(blockersAfter([](OldSetup &, Snapshot &s) { s.running = true; }), QStringList{"running"});
        QCOMPARE(blockersAfter([](OldSetup &, Snapshot &s) { s.resync = true; }), QStringList{"running"});
        QCOMPARE(blockersAfter([](OldSetup &, Snapshot &s) { s.lastRunOutcome = "failed"; }),
                 QStringList{"lastRunNotSuccess"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     QFile::remove(o.home + "/.local/state/rclone/bisync/gdrive_..home_Drive.path2.lst");
                 }),
                 QStringList{"noListings"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) { write(o.filters, "- changed/**\n"); }),
                 QStringList{"filtersChanged"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.rclone, "#!/bin/sh\necho other:\n");
                 }),
                 QStringList{"remoteMissing"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.service",
                           "[Service]\nType=oneshot\nExecStart=/usr/bin/unison profile\n");
                 }),
                 QStringList{"unreadableUnit"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.service",
                           (oldServiceText(o.home) + "Environment=RCLONE_CONFIG=/elsewhere\n").toUtf8());
                 }),
                 QStringList{"unknownDirective:Environment"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.service",
                           oldServiceText(o.home).replace("Nice=10", "Nice=5").toUtf8());
                 }),
                 QStringList{"differentValue:Nice"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.timer", "[Timer]\nOnBootSec=5min\nOnUnitActiveSec=30min\n");
                 }),
                 QStringList{"differentValue:OnUnitActiveSec"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     QDir().mkpath(o.paths.appConfigDir);
                     write(o.paths.appConfigDir + "/filters.txt", "left behind\n");
                 }),
                 QStringList{"targetExists"});
    }
    void takeoverMovesEverythingAndWritesTheManifest()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QVERIFY(plan.canApply());
        const QDateTime now(QDate(2026, 11, 9), QTime(10, 0), QTimeZone::UTC);
        QString error;
        QVERIFY2(applyTakeover(old.paths, plan, now, &error), qPrintable(error));

        // The config and the filters with their md5, byte for byte.
        const auto config = loadSyncConfig(old.paths.appConfigDir + "/config.json", &error);
        QVERIFY2(config.has_value(), qPrintable(error));
        QCOMPARE(config->filtersFile, old.paths.appConfigDir + "/filters.txt");
        QVERIFY(filtersUsable(config->filtersFile, &error));
        QCOMPARE(rcloneRunArguments(*config).mid(0, 5), (QStringList{"bisync", "gdrive:", old.home + "/Drive",
                                                                     "--workdir", plan.config.workDir}));

        // Every old file is in the backup, none is left where it ran from.
        const QDir backups(old.paths.backupRoot);
        const QStringList stamps = backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(stamps.size(), 1);
        const QString backup = backups.filePath(stamps.first());
        QVERIFY(QFile::exists(backup + "/manifest.json"));
        for (const QString &original : plan.found) {
            QVERIFY2(!QFileInfo::exists(original), qPrintable(original));
        }
        QVERIFY(QFile::exists(backup + "/systemd/drive-sync.service"));
        QVERIFY(QFile::exists(backup + "/bin/drive-sync.sh"));
        QVERIFY(QFile::exists(backup + "/rclone/gdrive-filtros.txt.md5"));
        QVERIFY(QFile::exists(backup + "/state/drive-sync/aviso"));

        // The packaged units are checked before any timer starts, and the
        // old filters move only after the second wait.
        QCOMPARE(systemctlCalls(dir.path()),
                 (QStringList{"systemctl --user is-enabled drive-sync.timer",
                              "systemctl --user is-active drive-sync.timer",
                              "systemctl --user is-enabled drive-sync-vigia.timer",
                              "systemctl --user is-active drive-sync-vigia.timer",
                              "systemctl --user stop drive-sync.timer drive-sync-vigia.timer",
                              "systemctl --user is-active drive-sync.service",
                              "systemctl --user disable drive-sync.timer drive-sync-vigia.timer",
                              "systemctl --user daemon-reload",
                              "systemctl --user show -p FragmentPath,DropInPaths drive-sync.service drive-sync.timer",
                              "systemctl --user is-active drive-sync.service",
                              "systemctl --user enable --now drive-sync.timer drive-sync-watchdog.timer"}));
        QVERIFY(!QFile::exists(takeoverLockPath(old.paths.appState)));
    }

    void takeoverRollsBackAtEveryStep()
    {
        // Each of the eleven systemctl calls fails in turn; then a move fails
        // halfway (12), the backup folder cannot be made (13), and the old
        // filters cannot move after the reload (14).
        for (int failAt = 1; failAt <= 14; ++failAt) {
            QTemporaryDir dir;
            OldSetup old = makeOldSetup(dir.path());
            const QString rcloneDir = QFileInfo(old.filters).path();
            old.paths.systemctl = takeoverSystemctl(dir.path(), failAt <= 11 ? failAt : 0, "inactive",
                                                    "/usr/lib/systemd/user",
                                                    failAt == 14 ? QString("chmod a-w '%1'").arg(rcloneDir) : ":");
            old.paths.serviceWaitMs = 300;
            const QString bin = old.paths.binDir;
            if (failAt == 12)  // the units move, then the scripts cannot leave their folder
                QFile::setPermissions(bin, QFile::ReadOwner | QFile::ExeOwner);
            if (failAt == 13) {
                // A file where the backup folder should go: the move cannot happen.
                QDir().mkpath(QFileInfo(old.paths.backupRoot).path());
                QVERIFY(write(old.paths.backupRoot, "in the way"));
            }
            const auto restore = qScopeGuard([&] {
                QFile::setPermissions(bin, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                QFile::setPermissions(rcloneDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            });
            const auto before = filesUnder(old.home);
            const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
            QVERIFY(plan.canApply());
            QString error;
            QVERIFY2(!applyTakeover(old.paths, plan, QDateTime::currentDateTimeUtc(), &error),
                     qPrintable(QString("step %1 did not fail").arg(failAt)));
            QVERIFY(!error.isEmpty());
            QCOMPARE(filesUnder(old.home), before);
            if (failAt >= 5)
                QVERIFY2(calls(dir.path()).contains("systemctl --user start drive-sync.timer drive-sync-vigia.timer"),
                         qPrintable(QString("old timers not back after step %1").arg(failAt)));
            if (failAt >= 7)
                QVERIFY2(calls(dir.path()).contains("systemctl --user enable drive-sync.timer drive-sync-vigia.timer"),
                         qPrintable(QString("old timers not enabled again after step %1").arg(failAt)));
            if (failAt == 11) {
                // One timer definition at a time: the packaged ones go, systemd
                // reloads with the old files back, and only then the old ones return.
                const QStringList made = systemctlCalls(dir.path());
                const auto at = [&](const QString &call) { return made.lastIndexOf("systemctl --user " + call); };
                QVERIFY(at("disable --now drive-sync.timer drive-sync-watchdog.timer") < at("daemon-reload"));
                QVERIFY(at("daemon-reload") < at("enable drive-sync.timer drive-sync-vigia.timer"));
            }
        }
    }

    void takeoverAndUndoNeverRunAtOnce()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QDir().mkpath(old.paths.appState);
        // Another drivesync, alive, holds the lock: apply and undo both refuse
        // and leave its lock alone.
        const QString other = dir.filePath("drivesync");
        QVERIFY(write(other, "#!/bin/sh\nsleep 30\n"));
        QFile::setPermissions(other, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QProcess holder;
        holder.start("/bin/sh", {other});
        QVERIFY(holder.waitForStarted());
        const QByteArray holderPid = QByteArray::number(holder.processId());
        QVERIFY(write(takeoverLockPath(old.paths.appState), holderPid));
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY2(error.contains("another"), qPrintable(error));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("another"), qPrintable(error));
        QFile lock(takeoverLockPath(old.paths.appState));
        QVERIFY(lock.open(QIODevice::ReadOnly));
        QCOMPARE(lock.readAll(), holderPid);
        lock.close();
        holder.kill();
        holder.waitForFinished();

        // A lock left by a takeover that died is replaced, and released after.
        QVERIFY(write(takeoverLockPath(old.paths.appState), "999999999\n"));
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error),
                 qPrintable(error));
        QVERIFY(!QFile::exists(takeoverLockPath(old.paths.appState)));
    }

    void takeoverLeavesEveryTimerOffWhenAFileCannotGoBack()
    {
        // The packaged unit is not the one systemd uses, and at that moment
        // the old units' folder turns read-only: they cannot go back, so the
        // old timers must not start without them.
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        const QString units = old.paths.userUnits;
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "inactive", "/somewhere/else",
                                                QString("chmod a-w '%1'").arg(units));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(units, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY2(error.contains("left off"), qPrintable(error));
        const QStringList after = systemctlCalls(dir.path());
        const int show = int(after.indexOf(QRegularExpression(".*FragmentPath.*")));
        QVERIFY(show > 0);
        for (const QString &call : after.mid(show + 1)) {
            QVERIFY2(!call.contains(" start ") && !call.contains(" enable "), qPrintable(call));
        }
        // The old filters never left; DriveSync's copy and config are gone, so
        // the window does not claim to run drive-sync.
        QVERIFY(filtersUsable(old.filters, nullptr));
        QVERIFY(!QFile::exists(old.paths.appConfigDir + "/config.json"));
        QVERIFY(!QFile::exists(old.paths.appConfigDir + "/filters.txt"));
    }

    void syncIsRefusedDuringATakeover()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QDir().mkpath(paths.appState);
        BackendPaths live = paths;
        live.proc = "/proc";
        // This test runs as drivesync_tests, a live drivesync process.
        QVERIFY(write(takeoverLockPath(live.appState), QByteArray::number(QCoreApplication::applicationPid())));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, live, o, e), 1);
        QVERIFY(err.contains("takeover"));
        QVERIFY(calls(dir.path()).isEmpty());

        // A takeover killed halfway leaves its lock behind: it does not count.
        QVERIFY(write(takeoverLockPath(live.appState), "999999999\n"));
        QCOMPARE(runSyncCommand({}, live, o, e), 0);
    }

    void takeoverStopsWhenARunStartsDuringTheStop()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "activating");
        old.paths.serviceWaitMs = 300;
        const auto before = filesUnder(old.home);
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY(error.contains("run"));
        QCOMPARE(filesUnder(old.home), before);
    }

    void undoRestoresTheOriginalFiles()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        const QStringList skip{".config/drivesync", ".local/share/drivesync", ".local/state/drivesync"};
        const auto before = filesUnder(old.home, skip);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QFile::remove(dir.filePath("calls.log"));
        PauseMark pause;
        pause.present = true;
        QVERIFY(writePause(pauseFilePath(old.paths.appState), pause));
        QVERIFY2(undoTakeover(old.paths, &error), qPrintable(error));
        QVERIFY(!readPause(pauseFilePath(old.paths.appState)).present);
        QCOMPARE(filesUnder(old.home, skip), before);
        QVERIFY(!QFile::exists(old.paths.appConfigDir + "/config.json"));
        QVERIFY(!QFile::exists(old.paths.appConfigDir + "/filters.txt"));
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).state, QString("found"));
        // One timer definition at a time: the packaged ones go before the old ones come back.
        QCOMPARE(systemctlCalls(dir.path()),
                 (QStringList{"systemctl --user is-active drive-sync.service",
                              "systemctl --user is-enabled drive-sync.timer",
                              "systemctl --user is-active drive-sync.timer",
                              "systemctl --user is-enabled drive-sync-watchdog.timer",
                              "systemctl --user is-active drive-sync-watchdog.timer",
                              "systemctl --user disable --now drive-sync.timer drive-sync-watchdog.timer",
                              "systemctl --user is-active drive-sync.service",
                              "systemctl --user daemon-reload",
                              "systemctl --user enable --now drive-sync.timer drive-sync-vigia.timer"}));

        // And it can be taken over again.
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()),
                               QDateTime::currentDateTimeUtc().addSecs(120), &error),
                 qPrintable(error));
    }

    void undoIsRefusedWhenFiltersChangedOrARunIsGoing()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(write(old.paths.appConfigDir + "/filters.txt", "- something new/**\n"));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY(error.contains("filters"));
        QVERIFY(QFile::exists(old.paths.appConfigDir + "/config.json"));

        QTemporaryDir busy;
        OldSetup other = makeOldSetup(busy.path());
        other.paths.systemctl = takeoverSystemctl(busy.path(), 0);
        QVERIFY(applyTakeover(other.paths, detectTakeover(other.paths, settledSnapshot()),
                              QDateTime::currentDateTimeUtc(), &error));
        other.paths.systemctl = takeoverSystemctl(busy.path(), 0, "active");
        QVERIFY(!undoTakeover(other.paths, &error));
        QVERIFY(error.contains("run"));
    }

    void movedFiltersKeepRealBisyncValid()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // Real rclone on two local folders, with no rclone config at all.
        QTemporaryDir dir;
        QDir d(dir.path());
        for (const char *folder : {"a", "b", "work", "old", "new"})
            d.mkpath(folder);
        QVERIFY(write(dir.filePath("a/one.txt"), "one\n"));
        QVERIFY(write(dir.filePath("old/filtros.txt"), "# Gerado pelo drive-sync.sh.\n- *.skip\n"));
        auto bisync = [&](const QStringList &extra) {
            QProcess p;
            p.start(rclone, QStringList{"--config", "/dev/null", "bisync", dir.filePath("a"), dir.filePath("b"),
                                        "--workdir", dir.filePath("work")} + extra);
            p.waitForFinished(60000);
            return p.exitCode();
        };
        QCOMPARE(bisync({"--filters-file", dir.filePath("old/filtros.txt"), "--resync", "-q"}), 0);
        QVERIFY(QFile::exists(dir.filePath("old/filtros.txt.md5")));

        QString error;
        QVERIFY2(copyFiltersWithMd5(dir.filePath("old/filtros.txt"), dir.filePath("new/filters.txt"), &error),
                 qPrintable(error));
        QVERIFY(QFile::remove(dir.filePath("old/filtros.txt")));
        QVERIFY(QFile::remove(dir.filePath("old/filtros.txt.md5")));

        SyncConfig config;
        config.remote = dir.filePath("a");
        config.localPath = dir.filePath("b");
        config.workDir = dir.filePath("work");
        config.filtersFile = dir.filePath("new/filters.txt");
        config.logFile = dir.filePath("sync.log");
        config.rcloneFlags = {"--config", "/dev/null", "-q"};
        QVERIFY(write(dir.filePath("a/two.txt"), "two\n"));
        QProcess run;
        run.start(rclone, runCommandPlan(config, &error));
        run.waitForFinished(60000);
        QCOMPARE(run.exitCode(), 0);
        QVERIFY(QFile::exists(dir.filePath("b/two.txt")));
        QVERIFY(QDir(dir.filePath("work")).entryList({"*-err"}).isEmpty());
    }
    void takeoverSetupOffersOnlyWhenItCanApply()
    {
        QTemporaryDir dir;
        const QString program = fakeDrivesync(
            dir.path(), R"({"ok":true,"error":"","state":"found","found":["/a/drive-sync.service"],"blockers":[]})");
        TakeoverSetup setup(program);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QCOMPARE(setup.found(), QStringList{"/a/drive-sync.service"});
        QCOMPARE(calls(dir.path()), QStringList{"drivesync takeover --check --json"});

        // The takeover itself, then a fresh look, which now says managed.
        write(dir.filePath("answer.json"), R"({"ok":true,"error":"","state":"managed","found":[],"blockers":[]})");
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("managed"));
        QVERIFY(calls(dir.path()).contains("drivesync takeover --apply --json"));

        // Nothing to take over: no offer, and accept does nothing.
        QTemporaryDir none;
        TakeoverSetup quiet(fakeDrivesync(none.path(), R"({"ok":true,"error":"","state":"none","found":[],"blockers":[]})"));
        quiet.check();
        QTRY_COMPARE(quiet.state(), QString("none"));
        quiet.accept();
        QCOMPARE(calls(none.path()), QStringList{"drivesync takeover --check --json"});
    }

    void takeoverSetupShowsBlockersAndFailures()
    {
        QTemporaryDir dir;
        const QString program = fakeDrivesync(
            dir.path(), R"({"ok":true,"error":"","state":"found","found":[],"blockers":["running","differentValue:Nice"]})");
        TakeoverSetup setup(program);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QCOMPARE(setup.blockers(), (QStringList{"running", "differentValue:Nice"}));

        QTemporaryDir failing;
        TakeoverSetup broken(fakeDrivesync(
            failing.path(), R"({"ok":true,"error":"","state":"found","found":["/a"],"blockers":[]})"));
        broken.check();
        QTRY_COMPARE(broken.state(), QString("offer"));
        write(failing.filePath("answer.json"),
              R"({"ok":false,"error":"could not stop the old timers: denied","state":"found","found":[],"blockers":[]})");
        broken.accept();
        QTRY_COMPARE(broken.state(), QString("failed"));
        QVERIFY(broken.detail().contains("denied"));

        // An answer that is not JSON is a failure too, never an offer.
        QTemporaryDir garbage;
        TakeoverSetup confused(fakeDrivesync(garbage.path(), "not json"));
        confused.check();
        QTRY_COMPARE(confused.state(), QString("failed"));
    }

    // Seen live on 2026-11-10: a run started while the dialog was open, and
    // the confirmed takeover ended as "failed: running".
    void takeoverSetupWaitsForARunAfterTheConfirmation()
    {
        QTemporaryDir dir;
        const QByteArray offer = R"({"ok":true,"error":"","state":"found","found":["/a"],"blockers":[]})";
        const QByteArray running = R"({"ok":false,"error":"running","state":"found","found":["/a"],"blockers":["running"]})";
        TakeoverSetup setup(fakeDrivesync(dir.path(), offer));
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        write(dir.filePath("answer.json"), running);
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QVERIFY(setup.pending());

        // Still running at the next look: still waiting, nothing applied.
        setup.check();
        QTRY_COMPARE(calls(dir.path()).size(), 3);
        QVERIFY(setup.pending());

        // The run ended: the next look applies without asking again.
        write(dir.filePath("answer.json"), offer);
        setup.check();
        QTRY_COMPARE(calls(dir.path()).count("drivesync takeover --apply --json"), 2);
        QVERIFY(!setup.pending());

        // Cancelled while waiting: the next offer is only an offer.
        QTemporaryDir other;
        TakeoverSetup cancelled(fakeDrivesync(other.path(), offer));
        cancelled.check();
        QTRY_COMPARE(cancelled.state(), QString("offer"));
        write(other.filePath("answer.json"), running);
        cancelled.accept();
        QTRY_VERIFY(cancelled.pending());
        cancelled.cancelPending();
        write(other.filePath("answer.json"), offer);
        cancelled.check();
        QTRY_COMPARE(cancelled.state(), QString("offer"));
        QCOMPARE(calls(other.path()).count("drivesync takeover --apply --json"), 1);

        // Blocked by something else than a run: the confirmation is dropped.
        QTemporaryDir third;
        TakeoverSetup dropped(fakeDrivesync(third.path(), offer));
        dropped.check();
        QTRY_COMPARE(dropped.state(), QString("offer"));
        write(third.filePath("answer.json"), running);
        dropped.accept();
        QTRY_VERIFY(dropped.pending());
        write(third.filePath("answer.json"),
              R"({"ok":true,"error":"","state":"found","found":["/a"],"blockers":["lastRunNotSuccess"]})");
        dropped.check();
        QTRY_COMPARE(dropped.blockers(), QStringList{"lastRunNotSuccess"});
        QVERIFY(!dropped.pending());
    }

    void backendControlsCallTheCommands()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        paths.self = fakeDrivesync(dir.path(), "{}");
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(backend.managed());

        backend.syncNow();
        backend.pauseFor(60);
        backend.pauseUntilTomorrow();
        backend.pauseUntilResumed();
        backend.resume();
        QTRY_COMPARE(calls(dir.path()).size(), 5);
        // The five run side by side, so they finish in any order.
        QStringList made = calls(dir.path());
        made.sort();
        QCOMPARE(made, (QStringList{"drivesync pause --json", "drivesync pause --minutes 60 --json",
                                    "drivesync pause --until-tomorrow --json", "drivesync resume --json",
                                    "drivesync sync --json"}));

        // The pause shows as soon as the mark is there.
        PauseMark mark;
        mark.present = true;
        // The mark keeps whole seconds.
        mark.since = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch(), QTimeZone::UTC);
        mark.until = mark.since.addSecs(3600);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        backend.refresh();
        QTRY_COMPARE(backend.status(), QString("paused"));
        QVERIFY(backend.paused());
        QCOMPARE(backend.pausedUntil(), mark.until);
        QVERIFY(!backend.canSyncNow());
    }

    void backendSyncNowReportsAFailure()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.self = fakeDrivesync(dir.path(), "{}", true);
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        backend.syncNow();
        QTRY_VERIFY(backend.message().contains("denied"));
    }
    void undoStopsWhenAFileCannotGoBack()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        const auto taken = filesUnder(old.home);

        // Something in the way: refused before anything changes.
        QVERIFY(write(old.home + "/.local/bin/drive-sync.sh", "someone else's\n"));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY(error.contains("in the way"));
        QVERIFY(QFile::remove(old.home + "/.local/bin/drive-sync.sh"));
        QCOMPARE(filesUnder(old.home), taken);

        // The scripts' folder cannot take them back: what already went back
        // returns to the backup, and DriveSync's timers start again.
        QFile::remove(dir.filePath("calls.log"));
        const QString bin = old.paths.binDir;
        QFile::setPermissions(bin, QFile::ReadOwner | QFile::ExeOwner);
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(bin, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("nothing was undone"), qPrintable(error));
        QCOMPARE(filesUnder(old.home), taken);
        // DriveSync's timers come back as they were: enabled, then started.
        QCOMPARE(systemctlCalls(dir.path()).mid(systemctlCalls(dir.path()).size() - 2),
                 (QStringList{"systemctl --user enable drive-sync.timer drive-sync-watchdog.timer",
                              "systemctl --user start drive-sync.timer drive-sync-watchdog.timer"}));
        QVERIFY(!systemctlCalls(dir.path()).contains("systemctl --user enable --now drive-sync.timer drive-sync-vigia.timer"));
    }

    void detectBlocksWhatSystemdReadsDifferently()
    {
        const auto blockersAfter = [](const std::function<void(OldSetup &)> &change) {
            QTemporaryDir dir;
            OldSetup old = makeOldSetup(dir.path());
            change(old);
            return detectTakeover(old.paths, settledSnapshot()).blockers;
        };
        // %h is the home for systemd, two literal characters for DriveSync.
        QVERIFY(blockersAfter([](OldSetup &o) {
                    write(o.paths.userUnits + "/drive-sync.service",
                          oldServiceText(o.home).replace(o.home + "/Drive", "%h/Drive").toUtf8());
                }).contains("unreadableUnit"));
        QVERIFY(blockersAfter([](OldSetup &o) {
                    write(o.paths.userUnits + "/drive-sync.service",
                          oldServiceText(o.home).replace("--drive-skip-gdocs", "'--drive-skip-gdocs'").toUtf8());
                }).contains("unreadableUnit"));
        QCOMPARE(blockersAfter([](OldSetup &o) { QDir().mkpath(o.paths.userUnits + "/drive-sync.service.d"); }),
                 QStringList{"dropIn"});
        QCOMPARE(blockersAfter([](OldSetup &o) { QDir().mkpath(o.paths.systemUserUnits + "/drive-sync.timer.d"); }),
                 QStringList{"dropIn"});
        QCOMPARE(blockersAfter([](OldSetup &o) { QFile::remove(o.paths.userUnits + "/drive-sync.timer"); }),
                 QStringList{"unreadableUnit"});
        QCOMPARE(blockersAfter([](OldSetup &o) {
                     write(o.paths.userUnits + "/drive-sync-vigia.service", "[Service]\nType=oneshot\nExecStart=/x/other\n");
                 }),
                 QStringList{"unknownDirective:drive-sync-vigia.service"});
        QCOMPARE(blockersAfter([](OldSetup &o) {
                     write(o.paths.userUnits + "/drive-sync-vigia.timer", "[Timer]\nOnStartupSec=10min\nOnUnitActiveSec=4h\n");
                 }),
                 QStringList{"differentValue:OnUnitActiveSec"});
    }

    void pauseOnlyCountsWhenManaged()
    {
        // After an undo the old units sync again: a leftover mark must not
        // say "paused" or hide a failure.
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.toLocalTime().addSecs(-3600), 480), now);
        src.pause.present = true;
        src.managed = false;
        const Snapshot snap = buildSnapshot(src);
        QVERIFY(!snap.paused);
        QVERIFY(snap.summary.status != "paused");
        QVERIFY(snap.canSyncNow);

        // An ended pause no longer stops "sync now".
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        PauseMark mark;
        mark.present = true;
        mark.until = QDateTime::currentDateTimeUtc().addSecs(-60);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, paths, o, e), 0);
    }

    void conditionFailsLikeSystemdOnCrashOr255()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        SyncConfig config = oldUnitConfig();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        config.runOnlyWhen = "exit 255";
        QCOMPARE(runConditionCommand(paths, config, now), 255);
        config.runOnlyWhen = "kill -9 $$";
        QCOMPARE(runConditionCommand(paths, config, now), 255);
        config.runOnlyWhen = "exit 3";
        QCOMPARE(runConditionCommand(paths, config, now), 1);
    }

    void refusedRunShowsAsAFailedRunWithItsCause()
    {
        // DriveSync's refusal is a CRITICAL line outside any run: the parser
        // makes it a failed run, so the window and the bar show its reason.
        const QDateTime start = QDateTime::currentDateTime().addSecs(-3600);
        QString log = successfulRun(start, 480);
        log += line(start.addSecs(900), "CRITICAL", "DriveSync did not start rclone: filters.txt.md5 is missing");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.last().outcome, QString("failed"));
        QVERIFY(runs.last().causeDetail.contains("filters.txt.md5 is missing"));
        const Snapshot snap = buildSnapshot(idleSources(log, QDateTime::currentDateTimeUtc()));
        QCOMPARE(snap.summary.status, QString("failed"));
        QVERIFY(snap.summary.causeDetail.contains("filters.txt.md5"));
    }

    void notifyNamesOnlyTheRunThatJustEnded()
    {
        // An old failed run is not the cause of this one.
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-3600)).toUtf8()));
        paths.systemctl = dir.filePath("systemctl");
        QVERIFY(write(paths.systemctl, QString("#!/bin/sh\necho @%1\n")
                                           .arg(QDateTime::currentSecsSinceEpoch() - 30).toUtf8()));
        QFile::setPermissions(paths.systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QCOMPARE(runNotifyCommand("exit-code", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        const QString sent = notifications(dir.path()).value(0);
        QVERIFY2(sent.contains("No detail"), qPrintable(sent));
        QVERIFY(!sent.contains("listings"));
    }

    void reminderDoesNotBringAPauseBack()
    {
        // A resume while notify-send shows the reminder wins.
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        NotifierPaths tools = fakeNotifier(dir.path());
        const QString mark = pauseFilePath(paths.appState);
        QVERIFY(write(tools.notifySend, QString("#!/bin/sh\nrm -f '%1'\necho 9\n").arg(mark).toUtf8()));
        PauseMark pause;
        pause.present = true;
        pause.since = QDateTime::currentDateTimeUtc().addSecs(-2 * 86400);
        QVERIFY(writePause(mark, pause));
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        QCOMPARE(runWatchdogCommand(paths, config, tools, QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(!readPause(mark).present);
    }

    void watchdogReportsABrokenConfig()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        QCOMPARE(runWatchdogCommand(paths, SyncConfig(), fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc(),
                                    "config.json is not a JSON object"),
                 0);
        QVERIFY(notifications(dir.path()).value(0).contains("config cannot be read"));
    }

    void backendFollowsATakeoverWithoutARestart()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.config = dir.filePath("config/config.json");
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(!backend.managed());

        SyncConfig config = oldUnitConfig();
        config.logFile = dir.filePath("drive-sync.log");
        config.localPath = dir.filePath("Drive");
        QVERIFY(saveSyncConfig(paths.config, config, nullptr));
        backend.reloadConfig();
        QTRY_VERIFY(backend.managed());
    }
    void takeoverWorksWithoutTheOldWatchdog()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        QVERIFY(QFile::remove(old.paths.userUnits + "/drive-sync-vigia.service"));
        QVERIFY(QFile::remove(old.paths.userUnits + "/drive-sync-vigia.timer"));
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error),
                 qPrintable(error));
        for (const QString &call : systemctlCalls(dir.path()))
            QVERIFY2(!call.contains("vigia"), qPrintable(call));
    }
    void detectBlocksPathsThatWouldMoveFirst()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        // The log inside the old state folder would leave with it.
        write(old.paths.userUnits + "/drive-sync.service",
              oldServiceText(old.home)
                  .replace(old.home + "/.local/state/drive-sync.log", old.paths.oldStateDir + "/rclone.log")
                  .toUtf8());
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).blockers, QStringList{"pathInsideMoved"});
    }

    void undoRefusesAfterATakeoverThatStoppedHalfway()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        const QString units = old.paths.userUnits;
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "inactive", "/somewhere/else",
                                                QString("chmod a-w '%1'").arg(units));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(units, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("by hand"), qPrintable(error));
    }

    void undoRefusedForARunChangesNothing()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        const auto taken = filesUnder(old.home);
        QFile::remove(dir.filePath("calls.log"));
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "active");
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY(error.contains("run"));
        QCOMPARE(filesUnder(old.home), taken);
        QCOMPARE(systemctlCalls(dir.path()), QStringList{"systemctl --user is-active drive-sync.service"});
    }
};

QTEST_MAIN(DrivesyncTests)
#include "drivesync_tests.moc"
