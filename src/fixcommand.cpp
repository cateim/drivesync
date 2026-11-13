#include "fixcommand.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <fcntl.h>
#include <sys/stat.h>

#include "controlcommands.h"
#include "fixstate.h"
#include "listingdiff.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

// Flags of a normal run that a base run must not take: the routine ones,
// and the config's own lock, log and verbosity, which the fix sets itself.
const QStringList kRoutineSwitches = {"--resilient", "--recover", "-v", "-vv", "-vvv", "--verbose"};
const QStringList kRoutineWithValue = {"--max-delete", "--conflict-resolve", "--check-sync",
                                       "--max-lock",   "--log-file",         "--log-level"};

QStringList withoutRoutine(const QStringList &flags) {
    QStringList kept;
    for (qsizetype i = 0; i < flags.size(); ++i) {
        const QString &flag = flags.at(i);
        const QString name = flag.section('=', 0, 0);
        if (kRoutineSwitches.contains(name))
            continue;
        if (kRoutineWithValue.contains(name)) {
            if (!flag.contains('='))
                ++i;  // its value is the next argument
            continue;
        }
        kept << flag;
    }
    return kept;
}

const QByteArray kDryRunCopy = ": Skipped copy as --dry-run";
const QByteArray kNotice = "NOTICE: ";
const QByteArray kResyncHeading = "Resync is copying files to";

}  // namespace

QStringList fixResyncArguments(const SyncConfig &config, bool dryRun, const QString &logFile) {
    SyncConfig base = config;
    base.rcloneFlags = withoutRoutine(config.rcloneFlags);
    QStringList arguments = rcloneRunArguments(base);
    arguments.removeLast();  // its "--log-file <file>" pair goes last, with the fix's log
    arguments.removeLast();
    arguments << "--resync" << "--resync-mode" << "newer" << "--check-sync=false" << "--max-lock" << "2m" << "-v"
              << "--log-file" << logFile;
    if (dryRun)
        arguments << "--dry-run";
    return arguments;
}

QString remoteFile(const QString &remote, const QString &path) {
    if (remote.endsWith(':') || remote.endsWith('/'))
        return remote + path;
    return remote + '/' + path;
}

QList<DryRunCopy> dryRunCopies(const QByteArray &log, int limit, int *total, int *toNotebook) {
    QList<DryRunCopy> notebook;
    QList<DryRunCopy> cloud;
    *total = 0;
    *toNotebook = 0;
    bool headingToNotebook = false;
    qsizetype from = 0;
    while (from < log.size()) {
        qsizetype end = log.indexOf('\n', from);
        if (end < 0)
            end = log.size();
        const QByteArray line = log.mid(from, end - from);
        from = end + 1;
        const qsizetype heading = line.indexOf(kResyncHeading);
        if (heading >= 0) {
            // "- Path1  Resync is copying files to  - Path2": cloud to notebook.
            headingToNotebook = line.mid(heading + kResyncHeading.size()).contains("Path2");
            continue;
        }
        const qsizetype copy = line.lastIndexOf(kDryRunCopy);
        const qsizetype notice = line.indexOf(kNotice);
        if (copy < 0 || notice < 0 || notice + kNotice.size() > copy)
            continue;
        ++*total;
        if (headingToNotebook)
            ++*toNotebook;
        QList<DryRunCopy> &side = headingToNotebook ? notebook : cloud;
        if (side.size() < limit)
            side.append({QString::fromUtf8(line.mid(notice + kNotice.size(), copy - notice - kNotice.size())),
                         headingToNotebook});
    }
    return (notebook + cloud).mid(0, limit);
}

QString fixConfigRefusal(const SyncConfig &config) {
    // Go's strconv.ParseBool, which rclone's flags use, reads these as false.
    static const QStringList falseWords = {"0", "f", "F", "false", "FALSE", "False"};
    for (const QString &flag : config.rcloneFlags) {
        if (flag.section('=', 0, 0) == "--drive-use-trash" && falseWords.contains(flag.section('=', 1)))
            return QString("the config turns off the Drive trash (%1), and a fix can remove files from the cloud: "
                           "take that flag out of %2 first")
                .arg(flag, defaultSyncConfigPath());
    }
    return QString();
}

namespace {

const QString kFixUnit = "drive-sync-fix.service";
constexpr int kSystemctlTimeoutMs = 15000;
// One file in the cloud: touch or delete. Generous, for a slow network.
constexpr int kRcloneFileTimeoutMs = 5 * 60 * 1000;
// A lock left by a killed run expires after --max-lock 2m; a little more.
constexpr int kLockWaitSeconds = 130;
constexpr int kServicePollSeconds = 5;
// rclone's exit code for "file not found".
constexpr int kRcloneNotFound = 4;

QString unitState(const BackendPaths &paths, const QString &unit, QString *error) {
    QByteArray output;
    if (!runProcess(paths.systemctl, {"--user", "show", "-p", "ActiveState", "--value", unit}, kSystemctlTimeoutMs,
                    &output, error))
        return QString();
    return QString::fromUtf8(output).trimmed();
}

bool unitBusy(const QString &state) {
    return state == "active" || state == "activating" || state == "deactivating" || state == "reloading";
}

QString startFixUnit(const BackendPaths &paths) {
    QString error;
    // --no-block: the step takes minutes; the state file says where it is.
    runProcess(paths.systemctl, {"--user", "start", "--no-block", kFixUnit}, kSystemctlTimeoutMs, nullptr, &error);
    return error;
}

// Moves the fix to step and starts the unit; puts the old state back when
// systemd says no, so the window keeps offering the same action.
QString advance(const BackendPaths &paths, const FixState &state, const QString &step) {
    FixState next = state;
    next.step = step;
    next.stepStarted = QDateTime::currentDateTimeUtc();
    next.error.clear();
    if (!writeFix(paths.appState, next))
        return QString("cannot write %1").arg(fixStatePath(paths.appState));
    const QString error = startFixUnit(paths);
    if (error.isEmpty())
        return QString();
    writeFix(paths.appState, state);
    return QString("could not start %1: %2").arg(kFixUnit, error);
}

// The config's runOnlyWhen (the old unit's ExecCondition): where it says no,
// as on the emergency home, ~/Drive is not the one to sync, and a resync
// would push it over the cloud.
QString runOnlyWhenRefusal(const SyncConfig &config) {
    if (config.runOnlyWhen.isEmpty())
        return QString();
    QString error;
    if (runProcess(QStandardPaths::findExecutable("sh"), {"-c", config.runOnlyWhen}, kSystemctlTimeoutMs, nullptr,
                   &error))
        return QString();
    return QString("the config's runOnlyWhen check says no (%1), so this is not the home drive-sync runs on")
        .arg(config.runOnlyWhen);
}

// Spec 5b: after --max-delete stopped a run, a resync would bring the
// deleted files back. Read from the newest finished run in the log.
QString maxDeleteRefusal(const SyncConfig &config) {
    bool truncated = false;
    const QString text = SyncLog::readRecent(config.logFile, kTailBytes, nullptr, &truncated);
    // Every record: a refusal per tick would push a short list past the stop.
    const QList<SyncRun> runs = SyncLog::parse(text, kStateRuns, truncated);
    if (!guardStopHolds(runs))
        return QString();
    // Edited filters after the stop: runs are refused before rclone, so the
    // list cannot show until the edit is undone.
    if (runs.last().causeKind == "filtersChanged")
        return "the last run that reached bisync stopped because it would delete too many files (--max-delete), and "
               "a fix would bring them back; undo the filters edit first: the next run then shows the deletions "
               "under D";
    return "the last run stopped because it would delete too many files (--max-delete); a fix would bring "
           "them back, so it is refused";
}

QString startFix(const BackendPaths &paths) {
    if (!paths.managed)
        return "a fix needs DriveSync to run drive-sync; take it over first";
    if (fixPresent(paths.appState))
        return "a fix is already under way; open DriveSync to follow it";
    if (takeoverLockHeld(paths.appState, paths.proc))
        return "a takeover is running; fix when it ends";
    QString error;
    const auto config = loadSyncConfig(paths.config, &error);
    if (!config)
        return error;
    for (const QString &refusal : {fixConfigRefusal(*config), runOnlyWhenRefusal(*config), maxDeleteRefusal(*config)})
        if (!refusal.isEmpty())
            return refusal;
    FixState state;
    state.step = "preparing";
    state.started = state.stepStarted = QDateTime::currentDateTimeUtc();
    // The mark goes first: from here on run-condition skips every tick.
    if (!writeFix(paths.appState, state))
        return QString("cannot write %1").arg(fixStatePath(paths.appState));
    error = startFixUnit(paths);
    if (error.isEmpty())
        return QString();
    clearFix(paths.appState);
    return QString("could not start %1: %2").arg(kFixUnit, error);
}

// Every question needs "cloud" or "notebook". Only the stored questions are
// looked up, so a path the fix never asked about is never acted on.
QString takeAnswers(const QString &file, FixState *state) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return QString("cannot read %1: %2").arg(file, in.errorString());
    const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
    if (!document.isObject())
        return QString("%1 is not a JSON object of answers").arg(file);
    const QJsonObject answers = document.object();
    for (FixQuestion &q : state->questions) {
        const QString answer = answers.value(q.path).toString();
        if (answer != "cloud" && answer != "notebook")
            return QString("%1 has no answer; every file needs \"cloud\" or \"notebook\"").arg(q.path);
        q.answer = answer;
    }
    return QString();
}

QString cancelFix(const BackendPaths &paths) {
    QString error;
    const QString unit = unitState(paths, kFixUnit, &error);
    if (unit.isEmpty())
        return QString("cannot ask systemd whether the fix is running: %1").arg(error);
    if (unitBusy(unit))
        return "a step of the fix is running; wait for it to end, then cancel";
    return clearFix(paths.appState) ? QString() : QString("cannot remove %1").arg(fixStatePath(paths.appState));
}

}  // namespace

QString retryFix(const BackendPaths &paths, const FixState &state) {
    QString error;
    const QString unit = unitState(paths, kFixUnit, &error);
    if (unit.isEmpty())
        return QString("cannot ask systemd whether the fix is running: %1").arg(error);
    if (unitBusy(unit))
        return "the fix is running already";
    // From a failed step, or from one the unit left halfway (a logout, a kill).
    const QString step = state.step == "failed" ? state.failedStep : fixWorking(state.step) ? state.step : QString();
    if (state.step == "failed" && !fixWorking(step))
        return QString("the fix cannot go on (%1); cancel it with drivesync fix --cancel").arg(state.error);
    if (!fixWorking(step))
        return QString("there is nothing to retry: the fix is at %1").arg(state.step);
    return advance(paths, state, step);
}

namespace {

// touch -c: a file that is not there stays not there.
bool touchExisting(const QString &path) {
    return ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), nullptr, 0) == 0;
}

// What a failed run wrote, past the generic "Bisync aborted" line.
QString lastErrorLine(const QByteArray &written) {
    static const QRegularExpression level(R"(\b(ERROR|CRITICAL)\s*:)");
    const QList<QByteArray> lines = written.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = QString::fromUtf8(*it).trimmed();
        const QRegularExpressionMatch m = level.match(line);
        if (!m.hasMatch() || line.contains("Bisync aborted"))
            continue;
        return line.mid(m.capturedEnd()).trimmed().left(200);
    }
    return QString();
}

QByteArray writtenSince(const QString &file, qint64 offset) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return QByteArray();
    // A log rotated in the meantime starts over.
    if (in.size() >= offset)
        in.seek(offset);
    return in.readAll();
}

// One bisync to its end. A lock left by a run that was killed expires 2
// minutes later (--max-lock 2m), so a "prior lock file found" in what this
// run wrote, never an older line of the log, is waited out once.
QString runBisync(const FixTools &tools, const QStringList &arguments, const QString &logFile) {
    for (int attempt = 0;; ++attempt) {
        const qint64 before = QFileInfo(logFile).size();
        QString error;
        // No time limit here: the unit's TimeoutStartSec bounds the step.
        if (runProcess(tools.rclone, arguments, -1, nullptr, &error))
            return QString();
        const QByteArray written = writtenSince(logFile, before);
        if (attempt == 0 && written.contains("prior lock file found")) {
            tools.wait(kLockWaitSeconds);
            continue;
        }
        const QString line = lastErrorLine(written);
        return line.isEmpty() ? error : line;
    }
}

// The config's flags that say how to reach the remote (--config and the
// --drive-* ones, with their values), for the one-file calls. Without them a
// touch or a delete could act on another root, or report "not found".
QStringList remoteFlags(const QStringList &flags) {
    QStringList kept;
    for (qsizetype i = 0; i < flags.size(); ++i) {
        const QString &flag = flags.at(i);
        const QString name = flag.section('=', 0, 0);
        if (name != "--config" && !name.startsWith("--drive-"))
            continue;
        kept << flag;
        // A value in the next argument (not a flag itself) goes along.
        if (!flag.contains('=') && i + 1 < flags.size() && !flags.at(i + 1).startsWith('-'))
            kept << flags.at(++i);
    }
    return kept;
}

// rclone on one file, and its exit code; -1 when it did not run or crashed.
int runRcloneFile(const FixTools &tools, const QStringList &arguments, QString *error) {
    QProcess process;
    process.start(tools.rclone, arguments);
    if (!process.waitForStarted(kRcloneFileTimeoutMs) || !process.waitForFinished(kRcloneFileTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        *error = "rclone did not answer";
        return -1;
    }
    *error = QString::fromUtf8(process.readAllStandardError()).trimmed().section('\n', -1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

// Spec section 5, step 1: nothing starts rclone while a normal run holds the
// listings. Then the dry run, into its own log.
QString prepareStep(const BackendPaths &paths, const SyncConfig &config, const FixTools &tools, FixState *state) {
    for (;;) {
        QString error;
        const QString service = unitState(paths, "drive-sync.service", &error);
        if (service.isEmpty())
            return QString("cannot ask systemd whether drive-sync is running: %1").arg(error);
        if (!unitBusy(service))
            break;
        tools.wait(kServicePollSeconds);
    }
    const QString dryLog = fixDryRunLogPath(paths.appState);
    QFile::remove(dryLog);
    const QString error = runBisync(tools, fixResyncArguments(config, true, dryLog), dryLog);
    if (!error.isEmpty())
        return error;
    QFile log(dryLog);
    int total = 0;
    int toNotebook = 0;
    if (log.open(QIODevice::ReadOnly))
        dryRunCopies(log.readAll(), 0, &total, &toNotebook);
    state->copies = total;
    state->step = "confirm";
    return QString();
}

// Steps 4 and 5: the base run into the main log, then the two listings
// compared. Notebook-newer files are touched so the normal run takes them
// over; cloud-newer and cloud-only files become questions.
QString baseStep(const SyncConfig &config, const FixTools &tools, FixState *state) {
    QString error = runBisync(tools, fixResyncArguments(config, false, config.logFile), config.logFile);
    if (!error.isEmpty())
        return error;
    const QString session = bisyncSessionName(config.remote, config.localPath);
    const QPair<QString, QString> files = currentListings(config.workDir, session);
    if (files.first.isEmpty())
        return QString("bisync left no %1.path1.lst and .path2.lst in %2").arg(session, config.workDir);
    const auto cloud = readListing(files.first, &error);
    if (!cloud)
        return error;
    const auto notebook = readListing(files.second, &error);
    if (!notebook)
        return error;
    const QList<Divergence> found = divergences(*cloud, *notebook);
    for (const Divergence &d : found)
        if (d.kind == "notUtf8")
            return QString("%1 has a name that is not UTF-8; rename it, then retry the fix").arg(d.path);
    state->touched.clear();
    state->questions.clear();
    for (const Divergence &d : found) {
        if (d.newer == "notebook") {
            if (touchExisting(config.localPath + "/" + d.path))
                state->touched << d.path;
            continue;
        }
        FixQuestion q;
        q.path = d.path;
        q.kind = d.kind;
        q.cloudSize = d.cloud.size;
        q.notebookSize = d.notebook.size;
        q.cloudTime = d.cloud.mtimeNs / 1000000000LL;
        q.notebookTime = d.notebook.mtimeNs / 1000000000LL;
        state->questions << q;
    }
    state->step = state->questions.isEmpty() ? "finishing" : "questions";
    return QString();
}

// Step 6: the answers, then a normal run with the routine flags and the md5
// check. Safe to run twice: a touch repeats, and a file already deleted
// answers "not found". A run that stopped at the max-delete guard comes back
// here with the user's confirmation ("drivesync deletes --allow").
QString finishStep(const QString &appState, const SyncConfig &config, const FixTools &tools, FixState *state) {
    QString error;
    for (const FixQuestion &q : std::as_const(state->questions)) {
        const QString remote = remoteFile(config.remote, q.path);
        if (q.answer == "cloud") {
            // Newer in the cloud from now on, so the normal run brings it down.
            if (runRcloneFile(tools, QStringList{"touch", "--no-create", remote} + remoteFlags(config.rcloneFlags),
                              &error)
                != 0)
                return QString("could not mark %1 in the cloud: %2").arg(q.path, error);
        } else if (q.answer == "notebook" && q.kind == "onlyCloud") {
            // The notebook does not have it: out of the cloud, into the Drive trash.
            // The Drive trash, whatever rclone.conf or RCLONE_DRIVE_USE_TRASH say.
            const int code = runRcloneFile(
                tools, QStringList{"deletefile", remote} + remoteFlags(config.rcloneFlags) + QStringList{"--drive-use-trash=true"},
                &error);
            if (code != 0 && code != kRcloneNotFound)
                return QString("could not remove %1 from the cloud: %2").arg(q.path, error);
        } else if (q.answer == "notebook") {
            touchExisting(config.localPath + "/" + q.path);
        } else {
            return QString("%1 has no answer; cancel the fix and start again").arg(q.path);
        }
    }
    QString refusal, note;
    const QStringList arguments =
        runPlanTakingMark(config, appState, tools.rclone, QDateTime::currentDateTimeUtc(), &refusal, &note);
    if (arguments.isEmpty())
        return refusal;
    if (!note.isEmpty())
        appendToLog(config.logFile, "INFO", note);
    error = runBisync(tools, arguments, config.logFile);
    if (!error.isEmpty())
        return error;
    state->step.clear();
    return QString();
}

}  // namespace

FixTools FixTools::defaults() {
    FixTools tools;
    tools.rclone = QStandardPaths::findExecutable("rclone");
    tools.wait = [](int seconds) { QThread::sleep(seconds); };
    return tools;
}

int runFixCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    const QString action = rest.value(0);
    const bool single = rest.size() == 1 && QStringList{"--start", "--confirm", "--cancel", "--retry"}.contains(action);
    if (!single && !(rest.size() == 2 && action == "--answers"))
        return commandAnswer(
            json, "usage: drivesync fix --start | --confirm | --answers <file.json> | --cancel | --retry [--json]", out,
            err);
    // One writer at a time: the unit and the watchdog's reminder take it too.
    const FixLock lock(paths.appState);
    if (action == "--start")
        return commandAnswer(json, startFix(paths), out, err);
    if (!fixPresent(paths.appState))
        return commandAnswer(json, action == "--cancel" ? QString() : QString("there is no fix under way"), out, err);
    FixState state = readFix(paths.appState);
    QString error;
    if (action == "--cancel") {
        error = cancelFix(paths);
    } else if (action == "--retry") {
        error = retryFix(paths, state);
    } else if (action == "--confirm") {
        error = state.step == "confirm" ? advance(paths, state, "base")
                                        : QString("there is nothing to confirm: the fix is at %1").arg(state.step);
    } else if (state.step != "questions") {
        error = QString("there is nothing to answer: the fix is at %1").arg(state.step);
    } else {
        error = takeAnswers(rest.at(1), &state);
        if (error.isEmpty())
            error = advance(paths, state, "finishing");
    }
    return commandAnswer(json, error, out, err);
}

int runFixUnit(const BackendPaths &paths, const SyncConfig &config, const FixTools &tools,
               const NotifierPaths &notify, const QString &configError) {
    // Read again after every step: the unit works until the fix waits for
    // the user, fails or ends, and a step never repeats on its own.
    for (;;) {
        FixState state;
        {
            const FixLock lock(paths.appState);
            state = readFix(paths.appState);
        }
        if (!fixWorking(state.step))
            return state.step == "failed" ? 1 : 0;
        const QString step = state.step;
        QString error = configError;
        // Checked before every step: a reboot can change the home in between.
        if (error.isEmpty())
            error = runOnlyWhenRefusal(config);
        if (error.isEmpty()) {
            if (step == "preparing")
                error = prepareStep(paths, config, tools, &state);
            else if (step == "base")
                error = baseStep(config, tools, &state);
            else
                error = finishStep(paths.appState, config, tools, &state);
        }
        if (!error.isEmpty()) {
            state.failedStep = step;
            // A final run that failed can take the new listings with it
            // (moved to -err): retrying it would fail forever.
            if (step == "finishing"
                && currentListings(config.workDir, bisyncSessionName(config.remote, config.localPath)).first.isEmpty())
                state.failedStep = "base";
            state.step = "failed";
            state.error = error;
            state.lastReminder = QDateTime::currentDateTimeUtc();
            {
                const FixLock lock(paths.appState);
                writeFix(paths.appState, state);
            }
            notifyFixNeedsYou(notify, state, true);
            return 1;
        }
        if (state.step.isEmpty()) {
            {
                const FixLock lock(paths.appState);
                clearFix(paths.appState);
            }
            notifyFixEnded(paths, notify);
            return 0;
        }
        state.stepStarted = QDateTime::currentDateTimeUtc();
        // A step that waits for the user says so at once; the watchdog
        // repeats it every hour from here.
        if (fixWaiting(state.step))
            state.lastReminder = state.stepStarted;
        bool written = false;
        {
            const FixLock lock(paths.appState);
            written = writeFix(paths.appState, state);
        }
        if (!written)
            return 1;
        if (fixWaiting(state.step))
            notifyFixNeedsYou(notify, state, false);
    }
}
