#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "pausestate.h"
#include "synclog.h"

// One state of drive-sync, decided in one place. The window, the
// "drivesync status" command and the bar widget all show what
// buildSnapshot() decides, so they can never disagree.

// A normal run writes about 30 KB to the log, a resync a few MB; the tail
// keeps dozens of normal runs without reading the whole history every time.
constexpr qint64 kTailBytes = 3 * 1024 * 1024;
// Runs the window lists.
constexpr int kMaxRuns = 40;
// Runs the state is decided from: every run in the tail. Forty runs are ten
// hours at the 15-minute interval, and a longer outage must still know when
// it began and when the last success was.
constexpr int kStateRuns = 1000000;
// rclone renews its lock and prints stats every minute, so an open run with
// nothing newer than this is no longer running.
constexpr int kAliveSeconds = 150;

// Everything the headline is decided from, gathered in one plain struct so
// the rules can be tested without systemd or a log.
struct SyncInputs {
    bool systemctlWorks = true;
    bool logFound = true;
    bool driveFound = true;
    QString serviceLoad;     // drive-sync.service LoadState
    QString serviceActive;   // ActiveState
    QString serviceResult;   // Result of its latest run
    QDateTime serviceStart;  // ExecMainStartTimestamp of that run
    QString timerActive;     // drive-sync.timer ActiveState
    QString timerFileState;  // UnitFileState: "enabled" starts it on boot
    bool resyncRunning = false;
    bool paused = false;     // a pause mark holds the runs
    bool running = false;    // a run is writing to the log right now
    QString lastOutcome;     // newest finished run in the log
    QDateTime lastStart;
    QString lastCauseKind;
    QString lastCauseDetail;
    QDateTime lastSuccess;
    QDateTime now;
    int conflicts = 0;
};

struct SyncSummary {
    QString status;  // see Backend::summarize
    QString causeKind;
    QString causeDetail;
    QStringList warnings;
};

// What a snapshot is built from: the parsed log and the raw answers of
// systemd, before any rule is applied.
struct SnapshotSources {
    QList<SyncRun> runs;              // SyncLog::parse output, oldest first
    bool logFound = false;
    bool systemctlWorks = true;
    QHash<QString, QString> service;  // parseShow block of drive-sync.service
    QHash<QString, QString> timer;    // parseShow block of drive-sync.timer
    bool resyncRunning = false;
    bool driveFound = true;
    int conflicts = 0;
    bool managed = false;             // DriveSync's own units run drive-sync
    PauseMark pause;
    QDateTime now;                    // UTC
};

struct Snapshot {
    SyncSummary summary;
    QString tone;                     // toneFor(summary.status)
    bool running = false;
    bool resync = false;
    bool canSyncNow = false;
    QVariantMap currentRun;           // Backend::runToMap(open run, "running"), empty when idle
    QVariantList runs;                // newest first, without the open run while running
    QDateTime lastSuccess;
    QDateTime lastRunStart;
    QDateTime lastRunEnd;
    QString lastRunOutcome;           // "success" | "failed" | "interrupted" | ""
    QDateTime failingSince;           // first run of the current failure, only when failed
    int typicalSeconds = 0;           // median of the last 10 successful non-resync runs
    int conflicts = 0;
    bool managed = false;
    bool paused = false;
    QDateTime pausedUntil;            // invalid while paused means until the user resumes
};

Snapshot buildSnapshot(const SnapshotSources &src);
// Version 2 of the "drivesync status --json" contract (version 1 plus
// managed, paused and pausedUntil). Every key is always
// present; times are Unix seconds, 0 when unknown. The bar widget reads it.
QJsonObject snapshotToJson(const Snapshot &s, const QDateTime &nextRun, const QDateTime &now);
// "ok" | "busy" | "warning" | "error": how urgent a headline is.
QString toneFor(const QString &status);
// The two systemctl calls every reader makes, so they cannot drift apart.
QStringList systemdShowArguments();
QStringList listTimersArguments();
// "systemctl show --timestamp=unix" prints "@1794076098", or nothing.
QDateTime fromUnixStamp(const QString &value);
