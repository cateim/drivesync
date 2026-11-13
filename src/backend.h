#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "blockeddeletes.h"
#include "snapshot.h"
#include "synclog.h"

// Where the Backend looks. Tests point every path at a temp directory and
// systemctl at a fake script.
struct BackendPaths {
    QString log;       // drive-sync's rclone log
    QString drive;     // the mirrored folder, where conflict copies land
    QString stateDir;  // drive-sync's state folder, which holds the resync pause mark
    QString proc;      // /proc, to check that the pid in the pause mark is drive-sync.sh
    QString systemctl;
    QString find;
    QString appState;  // DriveSync's own state folder (conflict count cache); empty writes nothing
    QString config;    // DriveSync's config; empty when there is none
    QString self;      // this program, for the commands the window runs
    bool managed = false;  // DriveSync runs drive-sync through its own units

    // Today's paths, or the config's log and folder once DriveSync owns drive-sync.

    static BackendPaths defaults();
};

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString tone READ tone NOTIFY changed)
    Q_PROPERTY(QString causeKind READ causeKind NOTIFY changed)
    Q_PROPERTY(QString causeDetail READ causeDetail NOTIFY changed)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
    Q_PROPERTY(QVariantMap currentRun READ currentRun NOTIFY changed)
    Q_PROPERTY(QDateTime lastSuccess READ lastSuccess NOTIFY changed)
    Q_PROPERTY(QDateTime nextRun READ nextRun NOTIFY changed)
    Q_PROPERTY(int typicalSeconds READ typicalSeconds NOTIFY changed)
    Q_PROPERTY(bool managed READ managed NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(QDateTime pausedUntil READ pausedUntil NOTIFY changed)
    Q_PROPERTY(bool canSyncNow READ canSyncNow NOTIFY changed)
    Q_PROPERTY(QVariantList runs READ runs NOTIFY runsChanged)
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY conflictsChanged)
    Q_PROPERTY(QString conflictError READ conflictError NOTIFY conflictsChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // The fix (fixstate.h), read from <appState>/fix.json: its step, what the
    // dry run would copy, and the files it asks about.
    Q_PROPERTY(QString fixStep READ fixStep NOTIFY fixChanged)
    Q_PROPERTY(int fixCopies READ fixCopies NOTIFY fixChanged)
    // At most 500 of the dry run's copies, as {path, toNotebook}, the copies
    // to the notebook first; and how many of all of them go to the notebook.
    Q_PROPERTY(QVariantList fixCopyList READ fixCopyList NOTIFY fixChanged)
    Q_PROPERTY(int fixCopiesToNotebook READ fixCopiesToNotebook NOTIFY fixChanged)
    Q_PROPERTY(QVariantList fixQuestions READ fixQuestions NOTIFY fixChanged)
    Q_PROPERTY(QString fixError READ fixError NOTIFY fixChanged)
    // The base run already ran: cancelling now throws it away, and the next
    // run fails until another fix.
    Q_PROPERTY(bool fixBaseDone READ fixBaseDone NOTIFY fixChanged)
    // When the newest run stopped at the max-delete guard (spec 5b): what it
    // would delete, per side, and at most 500 of them as {path,
    // deletedInCloud}, the ones gone from the cloud first. 0 and empty
    // otherwise.
    Q_PROPERTY(int blockedCount READ blockedCount NOTIFY blockedChanged)
    Q_PROPERTY(int blockedCloudCount READ blockedCloudCount NOTIFY blockedChanged)
    Q_PROPERTY(int blockedNotebookCount READ blockedNotebookCount NOTIFY blockedChanged)
    Q_PROPERTY(QVariantList blockedList READ blockedList NOTIFY blockedChanged)
    Q_PROPERTY(QString logPath READ logPath CONSTANT)
    Q_PROPERTY(QString drivePath READ drivePath CONSTANT)

public:
    explicit Backend(const BackendPaths &paths = BackendPaths::defaults(), QObject *parent = nullptr);
    ~Backend() override;

    QString status() const { return m_summary.status; }
    // "ok" | "busy" | "warning" | "error", the same grouping the bar shows.
    QString tone() const { return m_snapshot.tone; }
    QString causeKind() const { return m_summary.causeKind; }
    QString causeDetail() const { return m_summary.causeDetail; }
    QStringList warnings() const { return m_summary.warnings; }
    QVariantMap currentRun() const { return m_currentRun; }
    QDateTime lastSuccess() const { return m_lastSuccess; }
    QDateTime nextRun() const { return m_nextRun; }
    int typicalSeconds() const { return m_typicalSeconds; }
    bool managed() const { return m_snapshot.managed; }
    bool paused() const { return m_snapshot.paused; }
    QDateTime pausedUntil() const { return m_snapshot.pausedUntil; }
    bool canSyncNow() const { return m_snapshot.canSyncNow; }
    QVariantList runs() const { return m_runs; }
    QVariantList conflicts() const { return m_conflicts; }
    QString conflictError() const { return m_conflictError; }
    QString message() const { return m_message; }
    QString fixStep() const { return m_fix.step; }
    int fixCopies() const { return m_fix.copies; }
    QVariantList fixCopyList() const { return m_fixCopyList; }
    int fixCopiesToNotebook() const { return m_fixCopiesToNotebook; }
    QVariantList fixQuestions() const;
    QString fixError() const { return m_fix.error; }
    bool fixBaseDone() const {
        return m_fix.step == "questions" || m_fix.step == "finishing"
               || (m_fix.step == "failed" && m_fix.failedStep == "finishing");
    }
    int blockedCount() const { return m_blocked.count; }
    int blockedCloudCount() const { return m_blocked.cloudCount; }
    int blockedNotebookCount() const { return m_blocked.notebookCount; }
    QVariantList blockedList() const { return m_blockedList; }
    QString logPath() const { return m_paths.log; }
    QString drivePath() const { return m_paths.drive; }

    // Reads the log, asks systemd and looks for conflict copies again.
    Q_INVOKABLE void refresh();
    // Opens the file manager on a conflict copy, given relative to the drive.
    Q_INVOKABLE void showInFileManager(const QString &relativePath);
    // ~/Drive itself, as the bar's O opens it.
    Q_INVOKABLE void openDriveFolder();
    // The same commands the bar and the terminal use; a refusal or a failure
    // shows as the message.
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void pauseFor(int minutes);
    Q_INVOKABLE void pauseUntilTomorrow();
    Q_INVOKABLE void pauseUntilResumed();
    Q_INVOKABLE void resume();
    // "drivesync fix", step by step. answerFix takes "cloud" or "notebook"
    // for each question, in the order of fixQuestions, and hands them over
    // through <appState>/fix-answers.json. By position, not by name: a file
    // called "__proto__" or "constructor" would break a JavaScript object.
    Q_INVOKABLE void startFix();
    Q_INVOKABLE void confirmFix();
    Q_INVOKABLE void answerFix(const QStringList &answers);
    Q_INVOKABLE void cancelFix();
    Q_INVOKABLE void retryFix();
    // "drivesync deletes --allow <count>": count is what the user typed,
    // checked again by the command against the list in the log.
    Q_INVOKABLE void allowDeletes(int count);
    // Reads the config again after a takeover or an undo, so the window
    // follows the log and the controls it now owns without a restart.
    Q_INVOKABLE void reloadConfig();

    static SyncSummary summarize(const SyncInputs &in);
    // "systemctl show" output for several units: one property map per unit.
    static QList<QHash<QString, QString>> parseShow(const QByteArray &output);
    // The next elapse of the first timer in "systemctl list-timers -o json".
    static QDateTime parseNextElapse(const QByteArray &json);
    // True when the pid in drive-sync's pause mark is still drive-sync.sh.
    static bool resyncRunning(const QString &stateDir, const QString &proc);
    static QVariantMap runToMap(const SyncRun &run, const QString &outcome);

signals:
    void changed();
    void runsChanged();
    void conflictsChanged();
    void messageChanged();
    void fixChanged();
    void blockedChanged();

private:
    void readLog();
    void querySystemd();
    void scanConflicts();
    void recompute();
    void watchLog();
    void watchAppState();
    void updateFix(const FixState &fix);
    void updateBlocked(const QString &logText);
    void setMessage(const QString &message);
    void control(const QStringList &arguments);
    void run(const QString &program, const QStringList &arguments,
             std::function<void(bool ok, const QByteArray &output)> done);

    BackendPaths m_paths;
    QFileSystemWatcher m_watcher;
    // DriveSync's own state folder, where the fix writes its step. Watched
    // apart from the log, so a step change does not parse the log again.
    QFileSystemWatcher m_stateWatcher;
    QTimer m_stateDebounce;
    QTimer m_logDebounce;
    QTimer m_poll;
    QTimer m_conflictPoll;
    QTimer m_messageTimer;

    QList<SyncRun> m_parsed;
    bool m_logFound = false;
    bool m_systemctlWorks = true;
    bool m_systemdPending = false;
    bool m_scanPending = false;
    QHash<QString, QString> m_service;
    QHash<QString, QString> m_timer;
    QHash<QString, QString> m_fixUnit;
    QDateTime m_lastFinishedStart;

    Snapshot m_snapshot;
    SyncSummary m_summary;
    QVariantMap m_currentRun;
    QVariantList m_runs;
    QVariantList m_conflicts;
    QString m_conflictError;
    QDateTime m_lastSuccess;
    QDateTime m_nextRun;
    int m_typicalSeconds = 0;
    QString m_message;
    FixState m_fix;
    QVariantList m_fixCopyList;
    int m_fixCopiesToNotebook = 0;
    BlockedDeletes m_blocked;
    QVariantList m_blockedList;
};
