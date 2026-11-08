#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// What drive-sync's log says about each run. The log is plain rclone bisync
// output ("2026/11/07 15:28:19 INFO  : message", in local time), with
// multi-line stats and settings blocks whose continuation lines carry no
// timestamp.

// One file bisync found changed on one side during a run.
struct SyncChange {
    QString side;  // "cloud" (bisync's Path1), "notebook" (Path2) or "both"
    QString kind;  // "new", "modified", "deleted" or "conflict"
    QString path;  // relative to ~/Drive
};

// The "Path1: 9 changes: 9 new, 0 modified, 0 deleted" summary of one side.
struct SyncCounts {
    int added = 0;
    int modified = 0;
    int deleted = 0;
};

struct SyncRun {
    QDateTime start;
    QDateTime end;          // invalid while the run is open or after it was cut off
    QDateTime lastSeen;     // the run's newest log line; rclone writes one at least every minute
    QString outcome;        // "success", "failed", "interrupted" or "open"
    bool resync = false;
    QString phase;          // "listing", "comparing", "applying" or "updating"
    long long listed = -1;  // files listed so far, from the latest stats block
    SyncCounts cloud;
    SyncCounts notebook;
    QList<SyncChange> changes;
    int changesOmitted = 0;
    QStringList errors;
    QString causeKind;      // failed runs only: "lostListings", "outOfSync", "error" or "none"
    QString causeDetail;    // the error line behind "error"
};

namespace SyncLog {

// The runs in the text, oldest first, keeping the newest maxRuns. When the
// text starts in the middle of the log, pass startsMidway so the tail of a
// run whose start was cut off is skipped instead of read as a run of its own.
QList<SyncRun> parse(const QString &text, int maxRuns = 40, bool startsMidway = false);

// The last maxBytes of the file, from the first whole line. found is false
// when the file can't be read; truncated says whether the start was cut.
QString readTail(const QString &path, qint64 maxBytes, bool *found = nullptr, bool *truncated = nullptr);

}
