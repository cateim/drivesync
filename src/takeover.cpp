#include "takeover.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThread>
#include <functional>

#include "controlcommands.h"
#include "fixstate.h"
#include "pausestate.h"
#include "statuscommand.h"

namespace {

constexpr int kToolTimeoutMs = 15000;

// "[Section]" -> key -> values, in file order. systemd allows a key more than
// once, so every value is kept.
using UnitFile = QHash<QString, QList<QPair<QString, QString>>>;

UnitFile readUnit(const QString &path, bool *ok) {
    UnitFile unit;
    QFile file(path);
    *ok = file.open(QIODevice::ReadOnly);
    QString section;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        const int equals = line.indexOf('=');
        if (equals <= 0) {
            *ok = false;
            continue;
        }
        unit[section].append({line.left(equals).trimmed(), line.mid(equals + 1).trimmed()});
    }
    return unit;
}

// systemd's prefixes on an Exec line ("-" ignores failure, and so on).
QString withoutExecPrefix(QString value) {
    while (!value.isEmpty() && QString("-@:+!").contains(value.front()))
        value.remove(0, 1);
    return value;
}

// Checks a unit against the keys DriveSync knows: an unknown key is a
// blocker, and so is a known key with another value than the packaged unit.
void checkKeys(const UnitFile &unit, const QHash<QString, QHash<QString, QString>> &known, QStringList *blockers) {
    for (auto section = unit.cbegin(); section != unit.cend(); ++section) {
        const auto allowed = known.value(section.key());
        for (const auto &[key, value] : section.value()) {
            if (!allowed.contains(key)) {
                *blockers << "unknownDirective:" + key;
            } else if (!allowed.value(key).isEmpty() && allowed.value(key) != value) {
                *blockers << "differentValue:" + key;
            }
        }
    }
}

// QProcess::splitCommand knows double quotes only. systemd also reads single
// quotes, backslash escapes, %-specifiers and $VARIABLES, so a line with any
// of them could mean something else to systemd than to DriveSync.
bool readsLikeSystemd(const QString &value, const QString &forbidden) {
    for (const QChar c : forbidden)
        if (value.contains(c))
            return false;
    return true;
}

// Fills config from ExecStart: rclone bisync <remote> <local> and its flags.
bool readExecStart(const QString &value, SyncConfig *config, QString *oldFilters) {
    if (!readsLikeSystemd(value, "'%$\\"))
        return false;
    const QStringList tokens = QProcess::splitCommand(withoutExecPrefix(value));
    if (tokens.size() < 4 || QFileInfo(tokens.at(0)).fileName() != "rclone" || tokens.at(1) != "bisync"
        || tokens.at(2).startsWith('-') || tokens.at(3).startsWith('-'))
        return false;
    config->remote = tokens.at(2);
    config->localPath = tokens.at(3);
    const QHash<QString, QString *> own{{"--workdir", &config->workDir},
                                        {"--filters-file", oldFilters},
                                        {"--log-file", &config->logFile}};
    for (int i = 4; i < tokens.size(); ++i) {
        const QString &token = tokens.at(i);
        const int equals = token.indexOf('=');
        const QString name = equals > 0 ? token.left(equals) : token;
        if (own.contains(name)) {
            if (equals > 0) {
                *own.value(name) = token.mid(equals + 1);
            } else if (i + 1 < tokens.size()) {
                *own.value(name) = tokens.at(++i);
            } else {
                return false;
            }
        } else {
            config->rcloneFlags << token;
        }
    }
    return !config->workDir.isEmpty() && !oldFilters->isEmpty() && !config->logFile.isEmpty();
}

// ExecCondition=/usr/bin/sh -c "<text>" becomes runOnlyWhen=<text>.
bool readExecCondition(const QString &value, SyncConfig *config) {
    if (!readsLikeSystemd(value, "%$\\"))
        return false;
    const QStringList tokens = QProcess::splitCommand(withoutExecPrefix(value));
    const QString shell = tokens.isEmpty() ? QString() : QFileInfo(tokens.at(0)).fileName();
    if (tokens.size() != 3 || (shell != "sh" && shell != "bash") || tokens.at(1) != "-c")
        return false;
    config->runOnlyWhen = tokens.at(2);
    return true;
}

bool hasListings(const QString &workDir) {
    const QDir dir(workDir);
    return dir.entryList({"*.path1.lst"}, QDir::Files).size() == 1
           && dir.entryList({"*.path2.lst"}, QDir::Files).size() == 1;
}

bool remoteExists(const QString &rclone, const QString &remote) {
    QByteArray output;
    if (!runProcess(rclone, {"listremotes"}, kToolTimeoutMs, &output, nullptr))
        return false;
    for (const QByteArray &line : output.split('\n'))
        if (QString::fromUtf8(line).trimmed() == remote)
            return true;
    return false;
}

}  // namespace

TakeoverPaths TakeoverPaths::defaults() {
    const QString home = QDir::homePath();
    return {home + "/.config/systemd/user",
            home + "/.local/bin",
            home + "/.local/state/drive-sync",
            home + "/.config/drivesync",
            home + "/.local/share/drivesync/taken-over",
            QStandardPaths::findExecutable("systemctl"),
            QStandardPaths::findExecutable("rclone"),
            20 * 60 * 1000,
            "/usr/lib/systemd/user",
            home + "/.local/state/drivesync"};
}

TakeoverPlan detectTakeover(const TakeoverPaths &paths, const Snapshot &snapshot) {
    TakeoverPlan plan;
    if (loadSyncConfig(paths.appConfigDir + "/config.json", nullptr)) {
        plan.state = "managed";
        return plan;
    }
    const QString service = paths.userUnits + "/drive-sync.service";
    if (!QFile::exists(service)) {
        plan.state = "none";
        return plan;
    }
    plan.state = "found";

    bool serviceOk = false;
    const UnitFile unit = readUnit(service, &serviceOk);
    // The values the packaged unit uses; an empty one accepts any value
    // because the takeover carries it over.
    checkKeys(unit,
              {{"Unit", {{"Description", ""}}},
               {"Service",
                {{"Type", "oneshot"},
                 {"TimeoutStartSec", "2h"},
                 {"ExecCondition", ""},
                 {"ExecStart", ""},
                 {"ExecStopPost", ""},
                 {"Nice", "10"},
                 {"IOSchedulingClass", "idle"}}}},
              &plan.blockers);
    QStringList starts, conditions, stopPosts;
    for (const auto &[key, value] : unit.value("Service")) {
        if (key == "ExecStart")
            starts << value;
        else if (key == "ExecCondition")
            conditions << value;
        else if (key == "ExecStopPost")
            stopPosts << value;
    }
    bool readable = serviceOk && starts.size() == 1 && conditions.size() <= 1
                    && readExecStart(starts.first(), &plan.config, &plan.oldFiltersFile);
    if (readable && conditions.size() == 1)
        readable = readExecCondition(conditions.first(), &plan.config);
    for (const QString &stopPost : stopPosts)
        // The old notifier is replaced by "drivesync notify"; anything else would be lost.
        if (QFileInfo(QProcess::splitCommand(withoutExecPrefix(stopPost)).value(0)).fileName() != "drive-sync-aviso")
            plan.blockers << "unknownDirective:ExecStopPost";

    bool timerOk = false;
    const UnitFile timer = readUnit(paths.userUnits + "/drive-sync.timer", &timerOk);
    if (timerOk)
        checkKeys(timer,
                  {{"Unit", {{"Description", ""}}},
                   {"Timer", {{"OnBootSec", "5min"}, {"OnUnitActiveSec", "15min"}}},
                   {"Install", {{"WantedBy", "timers.target"}}}},
                  &plan.blockers);
    else
        readable = false;

    // The old watchdog's units, when they are there, must hold nothing the
    // packaged watchdog would drop.
    const QString vigiaService = paths.userUnits + "/drive-sync-vigia.service";
    if (QFile::exists(vigiaService)) {
        bool ok = false;
        const UnitFile vigia = readUnit(vigiaService, &ok);
        checkKeys(vigia,
                  {{"Unit", {{"Description", ""}}},
                   {"Service", {{"Type", "oneshot"}, {"ExecCondition", ""}, {"ExecStart", ""}}}},
                  &plan.blockers);
        bool knownStart = false;
        for (const auto &[key, value] : vigia.value("Service"))
            if (key == "ExecStart") {
                const QStringList tokens = QProcess::splitCommand(withoutExecPrefix(value));
                knownStart = tokens.size() == 2 && QFileInfo(tokens.at(0)).fileName() == "drive-sync-aviso"
                             && tokens.at(1) == "--vigia";
            }
        if (!ok || !knownStart)
            plan.blockers << "unknownDirective:drive-sync-vigia.service";
    }
    const QString vigiaTimer = paths.userUnits + "/drive-sync-vigia.timer";
    if (QFile::exists(vigiaTimer)) {
        bool ok = false;
        checkKeys(readUnit(vigiaTimer, &ok),
                  {{"Unit", {{"Description", ""}}},
                   {"Timer", {{"OnStartupSec", "10min"}, {"OnUnitActiveSec", "1h"}}},
                   {"Install", {{"WantedBy", "timers.target"}}}},
                  &plan.blockers);
        if (!ok)
            plan.blockers << "unknownDirective:drive-sync-vigia.timer";
    }

    // A drop-in applies to the packaged unit of the same name too, and one
    // with its own ExecStart would bypass "drivesync run" and its md5 check.
    for (const QString &dir : {paths.userUnits, paths.systemUserUnits})
        for (const QString &unitName : {QString("drive-sync.service"), QString("drive-sync.timer")})
            if (QFileInfo(dir + "/" + unitName + ".d").isDir())
                plan.blockers << "dropIn";

    if (fixPresent(paths.appState))
        plan.blockers.prepend("fixActive");
    if (snapshot.running || snapshot.resync || Backend::resyncRunning(paths.oldStateDir, "/proc"))
        plan.blockers.prepend("running");
    else if (snapshot.lastRunOutcome != "success")
        plan.blockers.prepend("lastRunNotSuccess");

    if (!readable) {
        plan.blockers << "unreadableUnit";
        plan.blockers.removeDuplicates();
        return plan;
    }
    plan.config.filtersFile = paths.appConfigDir + "/filters.txt";
    // The old units, scripts and state move first, while the old unit may
    // still run: what it reads must not move with them.
    for (const QString &path : {plan.oldFiltersFile, plan.config.workDir, plan.config.logFile})
        for (const QString &folder : {paths.oldStateDir, paths.userUnits, paths.binDir})
            if (path == folder || path.startsWith(folder + "/"))
                plan.blockers << "pathInsideMoved";
    if (!hasListings(plan.config.workDir))
        plan.blockers << "noListings";
    if (!filtersUsable(plan.oldFiltersFile, nullptr))
        plan.blockers << "filtersChanged";
    if (!remoteExists(paths.rclone, plan.config.remote))
        plan.blockers << "remoteMissing";
    if (QFile::exists(plan.config.filtersFile) || QFile::exists(plan.config.filtersFile + ".md5"))
        plan.blockers << "targetExists";
    plan.blockers.removeDuplicates();

    for (const QString &name : {QString("drive-sync.service"), QString("drive-sync.timer"),
                                QString("drive-sync-vigia.service"), QString("drive-sync-vigia.timer")})
        if (QFile::exists(paths.userUnits + "/" + name))
            plan.found << paths.userUnits + "/" + name;
    for (const QString &name : {QString("drive-sync.sh"), QString("drive-sync-aviso")})
        if (QFile::exists(paths.binDir + "/" + name))
            plan.found << paths.binDir + "/" + name;
    plan.found << plan.oldFiltersFile << plan.oldFiltersFile + ".md5";
    if (QFileInfo(paths.oldStateDir).isDir())
        plan.found << paths.oldStateDir;
    return plan;
}

namespace {

const QStringList kNewTimers{"drive-sync.timer", "drive-sync-watchdog.timer"};

// One step done, and how to take it back. A fatal undo stops the rollback:
// whatever comes before it (starting the old timers, above all) would be
// worse than leaving everything off.
struct Done {
    std::function<QString(bool *fatal)> undo;  // empty answer means it went fine
    bool always = false;  // DriveSync's own files: removed even after a fatal stop
};

bool systemctl(const TakeoverPaths &paths, const QStringList &arguments, QString *error) {
    return runProcess(paths.systemctl, QStringList{"--user"} + arguments, kToolTimeoutMs, nullptr, error);
}

// What systemctl prints for one unit (is-active, is-enabled), read from its
// output because both exit non-zero for an inactive or disabled unit.
QString unitAnswer(const TakeoverPaths &paths, const QString &verb, const QString &unit) {
    QByteArray output;
    runProcess(paths.systemctl, {"--user", verb, unit}, kToolTimeoutMs, &output, nullptr);
    return QString::fromUtf8(output).trimmed();
}

// Waits until no run is going.
bool waitForNoRun(const TakeoverPaths &paths, QString *error) {
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        const QString state = unitAnswer(paths, "is-active", "drive-sync.service");
        if (state.isEmpty()) {
            *error = "systemctl did not say whether a run is going";
            return false;
        }
        if (state != "active" && state != "activating" && state != "deactivating" && state != "reloading")
            return true;
        if (clock.elapsed() >= paths.serviceWaitMs) {
            *error = "a run is going; try again when it ends";
            return false;
        }
        QThread::msleep(quint32(qMin<qint64>(1000, paths.serviceWaitMs - clock.elapsed() + 1)));
    }
}

// A bisync outside the unit (drive-sync.sh resync) holds a lock file in the
// work dir that rclone renews every minute.
bool bisyncLockFresh(const QString &workDir) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QFileInfo &lock : QDir(workDir).entryInfoList({"*.lck"}, QDir::Files))
        if (lock.lastModified().toUTC().secsTo(now) < 150)
            return true;
    return false;
}

bool moveFile(const QString &from, const QString &to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    return QFileInfo(from).isDir() ? QDir().rename(from, to) : QFile::rename(from, to);
}

QString writeJson(const QString &path, const QJsonObject &object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString("cannot write %1").arg(path);
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit() ? QString() : QString("cannot write %1").arg(path);
}

// Where each old file goes inside the backup folder.
QList<QPair<QString, QString>> movesFor(const TakeoverPaths &paths, const TakeoverPlan &plan, const QString &backup) {
    QList<QPair<QString, QString>> moves;
    for (const QString &from : plan.found) {
        const QFileInfo info(from);
        QString to;
        if (from.startsWith(paths.userUnits + "/"))
            to = backup + "/systemd/" + info.fileName();
        else if (from.startsWith(paths.binDir + "/"))
            to = backup + "/bin/" + info.fileName();
        else if (from == paths.oldStateDir)
            to = backup + "/state/" + info.fileName();
        else
            to = backup + "/rclone/" + info.fileName();
        moves.append({from, to});
    }
    return moves;
}

// Moves each file, newest first, back where it came from. Stops at the
// first one that cannot go and returns it; done keeps what went back.
QString moveBack(const QList<QPair<QString, QString>> &moves, QList<QPair<QString, QString>> *done) {
    for (auto it = moves.crbegin(); it != moves.crend(); ++it) {
        if (!moveFile(it->second, it->first))
            return it->first;
        done->append(*it);
    }
    return {};
}

// The newest takeover not undone yet, or an empty path.
QString newestBackup(const TakeoverPaths &paths, QJsonObject *manifest) {
    QStringList stamps = QDir(paths.backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (auto it = stamps.crbegin(); it != stamps.crend(); ++it) {
        const QString folder = paths.backupRoot + "/" + *it;
        QFile file(folder + "/manifest.json");
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        if (!object.value("undone").toBool()) {
            *manifest = object;
            return folder;
        }
    }
    return {};
}

// Puts the old timers back the way they were: enabled ones enabled, running ones started.
QString restoreOldTimers(const TakeoverPaths &paths, const QStringList &enabled, const QStringList &active) {
    QStringList problems;
    QString e;
    if (!enabled.isEmpty() && !systemctl(paths, QStringList{"enable"} + enabled, &e))
        problems << e;
    if (!active.isEmpty() && !systemctl(paths, QStringList{"start"} + active, &e))
        problems << e;
    return problems.join("; ");
}

// Takes the takeover lock, shared by apply and undo, so they never run at
// once. A lock whose process is gone is replaced.
bool takeLock(const QString &appState, QString *error) {
    const QString path = takeoverLockPath(appState);
    QDir().mkpath(appState);
    if (QFileInfo::exists(path)) {
        if (takeoverLockHeld(appState, "/proc")) {
            *error = "another takeover or undo is running";
            return false;
        }
        QFile::remove(path);
    }
    QFile lock(path);
    if (!lock.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || lock.write(QByteArray::number(QCoreApplication::applicationPid()) + "\n") <= 0) {
        *error = "cannot take the takeover lock " + path;
        return false;
    }
    return true;
}

// Removes the lock only when it is still ours.
void releaseLock(const QString &appState) {
    QFile lock(takeoverLockPath(appState));
    if (lock.open(QIODevice::ReadOnly) && lock.readAll().trimmed() == QByteArray::number(QCoreApplication::applicationPid())) {
        lock.close();
        lock.remove();
    }
}

// The packaged units must be the ones systemd uses, with no drop-in on top.
QString checkPackagedUnits(const TakeoverPaths &paths) {
    QByteArray output;
    QString error;
    if (!runProcess(paths.systemctl,
                    {"--user", "show", "-p", "FragmentPath,DropInPaths", "drive-sync.service", "drive-sync.timer"},
                    kToolTimeoutMs, &output, &error))
        return "systemctl show failed: " + error;
    const auto units = Backend::parseShow(output);
    if (units.size() != 2)
        return "systemctl show did not describe both units";
    for (const auto &unit : units) {
        const QString fragment = unit.value("FragmentPath");
        if (!fragment.startsWith(paths.packagedUnits + "/"))
            return QString("systemd still uses %1, not DriveSync's unit").arg(fragment.isEmpty() ? "?" : fragment);
        if (!unit.value("DropInPaths").isEmpty())
            return "a drop-in still applies: " + unit.value("DropInPaths");
    }
    return {};
}

}  // namespace

bool copyFiltersWithMd5(const QString &from, const QString &to, QString *error) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (QFile::exists(to) || QFile::exists(to + ".md5")) {
        *error = QString("%1 already exists").arg(to);
        return false;
    }
    if (!QFile::copy(from, to) || !QFile::copy(from + ".md5", to + ".md5")) {
        QFile::remove(to);
        *error = QString("cannot copy %1 and its md5 to %2").arg(from, to);
        return false;
    }
    if (!filtersUsable(to, error)) {
        QFile::remove(to);
        QFile::remove(to + ".md5");
        return false;
    }
    return true;
}

bool applyTakeover(const TakeoverPaths &paths, const TakeoverPlan &plan, const QDateTime &now, QString *error) {
    if (!plan.canApply()) {
        *error = plan.blockers.isEmpty() ? QString("nothing to take over") : plan.blockers.join(", ");
        return false;
    }
    // "drivesync sync" and an undo refuse while this lock is held.
    if (!takeLock(paths.appState, error))
        return false;
    const auto unlock = qScopeGuard([&] { releaseLock(paths.appState); });

    QList<Done> done;
    // Takes back every step done so far, newest first, and stops at a fatal
    // one; what could not be taken back is added to the error.
    // After a fatal one only DriveSync's own files go, so the window does not
    // claim to run drive-sync while every timer is off.
    auto rollBack = [&](const QString &why) {
        QStringList left;
        bool stopped = false;
        for (auto it = done.crbegin(); it != done.crend(); ++it) {
            if (stopped && !it->always)
                continue;
            bool fatal = false;
            if (const QString problem = it->undo(&fatal); !problem.isEmpty())
                left << problem;
            stopped = stopped || fatal;
        }
        *error = left.isEmpty() ? why : why + "; and while undoing: " + left.join("; ");
        return false;
    };

    QString why;
    // The old timers that exist: a setup without the old watchdog has one.
    QStringList oldTimers;
    for (const QString &file : plan.found)
        if (file.startsWith(paths.userUnits + "/") && file.endsWith(".timer"))
            oldTimers << QFileInfo(file).fileName();
    QStringList wasEnabled, wasActive;
    for (const QString &timer : oldTimers) {
        const QString enabled = unitAnswer(paths, "is-enabled", timer);
        const QString active = unitAnswer(paths, "is-active", timer);
        if (enabled.isEmpty() || active.isEmpty())
            return rollBack("systemctl did not say how " + timer + " is");
        if (enabled == "enabled")
            wasEnabled << timer;
        if (active == "active")
            wasActive << timer;
    }

    done.append({[&](bool *) {
        QString e;
        return wasActive.isEmpty() || systemctl(paths, QStringList{"start"} + wasActive, &e) ? QString() : e;
    }});
    if (!systemctl(paths, QStringList{"stop"} + oldTimers, &why))
        return rollBack("could not stop the old timers: " + why);

    if (!waitForNoRun(paths, &why))
        return rollBack(why);
    // A run may have changed things between the plan and the stop, and a
    // resync from drive-sync.sh runs outside the unit.
    if (Backend::resyncRunning(paths.oldStateDir, "/proc") || bisyncLockFresh(plan.config.workDir))
        return rollBack("a bisync is running outside the unit; try again when it ends");
    if (!filtersUsable(plan.oldFiltersFile, &why))
        return rollBack(why);
    if (!hasListings(plan.config.workDir))
        return rollBack("the bisync listings are gone");

    if (!copyFiltersWithMd5(plan.oldFiltersFile, plan.config.filtersFile, &why))
        return rollBack(why);
    done.append({[&](bool *) {
        QFile::remove(plan.config.filtersFile);
        QFile::remove(plan.config.filtersFile + ".md5");
        return QString();
    }, true});

    const QString configFile = paths.appConfigDir + "/config.json";
    if (!saveSyncConfig(configFile, plan.config, &why))
        return rollBack(why);
    done.append({[&](bool *) { return QFile::remove(configFile) ? QString() : "could not remove " + configFile; },
                 true});

    done.append({[&](bool *) { return restoreOldTimers(paths, wasEnabled, {}); }});
    if (!systemctl(paths, QStringList{"disable"} + oldTimers, &why))
        return rollBack("could not disable the old timers: " + why);

    // The manifest goes first, so a crash halfway still says where each file went.
    QString backup = paths.backupRoot + "/" + now.toLocalTime().toString("yyyyMMdd-HHmmss");
    if (QFileInfo::exists(backup))
        backup += "-" + QString::number(now.toMSecsSinceEpoch());
    const auto moves = movesFor(paths, plan, backup);
    QJsonArray moved;
    for (const auto &[from, to] : moves)
        moved.append(QJsonObject{{"from", from}, {"to", to}});
    if (!QDir().mkpath(backup))
        return rollBack("cannot create " + backup);
    if (const QString problem = writeJson(backup + "/manifest.json",
                                          {{"version", 1},
                                           {"takenOverAt", now.toSecsSinceEpoch()},
                                           {"moved", moved},
                                           {"oldTimersEnabled", QJsonArray::fromStringList(wasEnabled)},
                                           {"oldTimersActive", QJsonArray::fromStringList(wasActive)},
                                           {"undone", false}});
        !problem.isEmpty()) {
        QDir(backup).removeRecursively();
        return rollBack(problem);
    }
    QList<QPair<QString, QString>> movedSoFar;
    done.append({[&](bool *fatal) {
        QList<QPair<QString, QString>> back;
        const QString stuck = moveBack(movedSoFar, &back);
        QString e;
        systemctl(paths, {"daemon-reload"}, &e);
        if (!stuck.isEmpty()) {
            // Without that file the old units cannot run safely: stop here,
            // with every timer off, and say where everything is.
            *fatal = true;
            QFile file(backup + "/manifest.json");
            if (file.open(QIODevice::ReadOnly)) {
                QJsonObject stuckManifest = QJsonDocument::fromJson(file.readAll()).object();
                file.close();
                stuckManifest.insert("stuck", true);
                writeJson(backup + "/manifest.json", stuckManifest);
            }
            return QString("%1 could not go back from %2; every drive-sync timer is left off").arg(stuck, backup);
        }
        QDir(backup).removeRecursively();
        return QString();
    }});
    // Everything but the old filters, which the old unit still reads until systemd reloads.
    const QString oldFilters = plan.oldFiltersFile;
    for (const auto &move : moves) {
        if (move.first == oldFilters || move.first == oldFilters + ".md5")
            continue;
        if (!moveFile(move.first, move.second))
            return rollBack(QString("cannot move %1 to %2").arg(move.first, move.second));
        movedSoFar.append(move);
    }

    if (!systemctl(paths, {"daemon-reload"}, &why))
        return rollBack("systemd did not reload: " + why);
    if (const QString problem = checkPackagedUnits(paths); !problem.isEmpty())
        return rollBack(problem);
    // A run started by hand before the reload used the old unit; let it end
    // before its filters move.
    if (!waitForNoRun(paths, &why))
        return rollBack(why);
    for (const auto &move : moves) {
        if (move.first != oldFilters && move.first != oldFilters + ".md5")
            continue;
        if (!moveFile(move.first, move.second))
            return rollBack(QString("cannot move %1 to %2").arg(move.first, move.second));
        movedSoFar.append(move);
    }

    done.append({[&](bool *) {
        QString e;
        return systemctl(paths, QStringList{"disable", "--now"} + kNewTimers, &e) ? QString() : e;
    }});
    if (!systemctl(paths, QStringList{"enable", "--now"} + kNewTimers, &why))
        return rollBack("could not enable DriveSync's timers: " + why);
    return true;
}

bool undoTakeover(const TakeoverPaths &paths, QString *error) {
    // The old units know nothing of a fix: going back halfway would leave
    // its base run or its questions to a drive-sync that cannot finish them.
    if (fixPresent(paths.appState)) {
        *error = "a fix is under way; finish it or cancel it (drivesync fix --cancel) first";
        return false;
    }
    if (!takeLock(paths.appState, error))
        return false;
    const auto unlock = qScopeGuard([&] { releaseLock(paths.appState); });
    QJsonObject manifest;
    const QString backup = newestBackup(paths, &manifest);
    if (backup.isEmpty()) {
        *error = "there is no takeover to undo";
        return false;
    }
    if (manifest.value("stuck").toBool()) {
        *error = QString("the last takeover stopped halfway; finish it by hand, following %1/manifest.json").arg(backup);
        return false;
    }
    QList<QPair<QString, QString>> moves;
    QString oldFilters;
    for (const QJsonValue &item : manifest.value("moved").toArray()) {
        const QString from = item.toObject().value("from").toString();
        const QString to = item.toObject().value("to").toString();
        moves.append({from, to});
        if (to.startsWith(backup + "/rclone/") && !to.endsWith(".md5"))
            oldFilters = to;
    }
    const QString filters = paths.appConfigDir + "/filters.txt";
    // The md5 too: it is what bisync compares, and a hand-made resync
    // rewrites it even when the text ends up the same.
    QFile current(filters), saved(oldFilters), currentMd5(filters + ".md5"), savedMd5(oldFilters + ".md5");
    if (!current.open(QIODevice::ReadOnly) || !saved.open(QIODevice::ReadOnly)
        || !currentMd5.open(QIODevice::ReadOnly) || !savedMd5.open(QIODevice::ReadOnly)
        || current.readAll() != saved.readAll() || currentMd5.readAll() != savedMd5.readAll()) {
        *error = "the filters changed since the takeover, so the old setup would need a resync";
        return false;
    }
    // Nothing may sit where an old file goes back, and every saved file must be there.
    for (const auto &[from, to] : moves) {
        if (QFileInfo::exists(from)) {
            *error = QString("%1 is in the way; move it and undo again").arg(from);
            return false;
        }
        if (!QFileInfo::exists(to)) {
            *error = QString("%1 is missing from the backup").arg(to);
            return false;
        }
    }

    QString why;
    TakeoverPaths noWait = paths;
    noWait.serviceWaitMs = 0;
    if (!waitForNoRun(noWait, &why)) {
        *error = "a run is going; undo when it ends";
        return false;
    }
    // How DriveSync's timers are now, so a refusal leaves them exactly so.
    QStringList newEnabled, newActive;
    for (const QString &timer : kNewTimers) {
        if (unitAnswer(paths, "is-enabled", timer) == "enabled")
            newEnabled << timer;
        if (unitAnswer(paths, "is-active", timer) == "active")
            newActive << timer;
    }
    auto restoreNew = [&] {
        const QString problem = restoreOldTimers(paths, newEnabled, newActive);
        return problem.isEmpty() ? QString() : "; and DriveSync's timers did not come back: " + problem;
    };
    if (!systemctl(paths, QStringList{"disable", "--now"} + kNewTimers, &why)) {
        *error = "could not stop DriveSync's timers: " + why + restoreNew();
        return false;
    }
    // The timer may have fired between the check and the stop.
    if (!waitForNoRun(noWait, &why)) {
        *error = "a run is going; undo when it ends" + restoreNew();
        return false;
    }

    QList<QPair<QString, QString>> back;
    if (const QString stuck = moveBack(moves, &back); !stuck.isEmpty()) {
        // Put back into the backup what already left it, so DriveSync's units
        // are the only ones again, and start them.
        QList<QPair<QString, QString>> again;
        for (const auto &[from, to] : back)
            again.append({to, from});
        QList<QPair<QString, QString>> ignored;
        moveBack(again, &ignored);
        systemctl(paths, {"daemon-reload"}, &why);
        *error = QString("%1 could not go back, so nothing was undone; DriveSync still runs drive-sync").arg(stuck)
                 + restoreNew();
        return false;
    }
    QStringList problems;
    const QString stamp = QFileInfo(backup).fileName();
    if (!QFile::rename(paths.appConfigDir + "/config.json", paths.appConfigDir + "/config.json.undone-" + stamp))
        problems << "could not put " + paths.appConfigDir + "/config.json away; remove it by hand";
    for (const QString &copy : {filters, filters + ".md5"})
        if (QFile::exists(copy) && !QFile::remove(copy))
            problems << "could not remove " + copy;
    // The old units never look at a pause; leaving the mark would show "paused" while they sync.
    if (!clearPause(pauseFilePath(paths.appState)))
        problems << "could not remove the pause mark";
    manifest.insert("undone", true);
    if (const QString problem = writeJson(backup + "/manifest.json", manifest); !problem.isEmpty())
        problems << problem;

    if (!systemctl(paths, {"daemon-reload"}, &why))
        problems << "systemd did not reload: " + why;
    QStringList enabled, active;
    for (const QJsonValue &v : manifest.value("oldTimersEnabled").toArray())
        enabled << v.toString();
    for (const QJsonValue &v : manifest.value("oldTimersActive").toArray())
        active << v.toString();
    if (enabled == active && !enabled.isEmpty()) {
        if (!systemctl(paths, QStringList{"enable", "--now"} + enabled, &why))
            problems << "could not enable the old timers: " + why;
    } else if (const QString problem = restoreOldTimers(paths, enabled, active); !problem.isEmpty()) {
        problems << problem;
    }
    if (!problems.isEmpty()) {
        *error = "undone, but check by hand: " + problems.join("; ");
        return false;
    }
    return true;
}

int runTakeoverCommand(const QStringList &arguments, const TakeoverPaths &paths, const BackendPaths &backend,
                       QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    if (rest.size() != 1 || !QStringList{"--check", "--apply", "--undo"}.contains(rest.first())) {
        err << "usage: drivesync takeover --check | --apply | --undo [--json]\n";
        return 1;
    }
    const QString action = rest.first();
    QString error;
    bool ok = true;
    TakeoverPlan plan;
    if (action == "--undo") {
        ok = undoTakeover(paths, &error);
    } else {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        plan = detectTakeover(paths, buildSnapshot(gatherSources(backend, now, 5000, nullptr)));
        if (action == "--apply")
            ok = applyTakeover(paths, plan, now, &error);
    }
    if (json) {
        out << QJsonDocument(QJsonObject{{"ok", ok},
                                         {"error", error},
                                         {"state", plan.state},
                                         {"found", QJsonArray::fromStringList(plan.found)},
                                         {"blockers", QJsonArray::fromStringList(plan.blockers)}})
                   .toJson(QJsonDocument::Compact)
            << "\n";
    } else if (action == "--check") {
        out << "state: " << plan.state << "\n";
        for (const QString &file : plan.found)
            out << "moves: " << file << "\n";
        for (const QString &blocker : plan.blockers)
            out << "blocked: " << blocker << "\n";
        if (plan.state == "found")
            out << "runs: rclone " << rcloneRunArguments(plan.config).join(' ') << "\n";
    } else if (!ok) {
        err << "drivesync: " << error << "\n";
    }
    out.flush();
    err.flush();
    return ok && (action != "--check" || plan.canApply() || plan.state == "managed") ? 0 : 1;
}
