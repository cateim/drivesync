#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <optional>

struct BackendPaths;  // backend.h, which includes this header

// When --max-delete stops a run (spec section 5b), the run deleted nothing
// and its own log lines say what it would have deleted: one "File was
// deleted" line per file, both sides, then "Safety abort: too many deletes
// (>5%, 11 of 21) on Path2 ...". bisync checks each side against that
// side's count in the last listing, Path1 first (rclone 1.75.2).

struct BlockedDelete {
    QString path;                 // relative to ~/Drive
    bool deletedInCloud = false;  // Path1: gone from the cloud, so it goes from the notebook (for good)
};

struct BlockedDeletes {
    QDateTime runStart;  // the run that stopped (log time, as SyncRun.start)
    int count = 0;       // every deletion of that run, both sides
    int cloudCount = 0, notebookCount = 0;
    int guardCount = 0, guardTotal = 0;  // "11 of 21" from the abort line
    QList<BlockedDelete> list;           // at most limit, the ones that go from the notebook first
};

// The deletion lines of the newest run in the text (from its "Synching
// Path1" line on), whatever it ended with, at any level: a dry run writes
// them as NOTICE. Fills count, cloudCount, notebookCount and list.
BlockedDeletes deletionsInNewestRun(const QString &logText, int limit);

// The newest run in the log text, when it stopped at the max-delete guard.
// A run after it, even one DriveSync refused or one under way, holds nothing
// back.
std::optional<BlockedDeletes> blockedDeletes(const QString &logText, int limit);

// What the user confirmed, kept for one run in <appState>/allow-deletes.json
// ("drivesync deletes --allow"). The run that uses it raises --max-delete
// just enough for these deletions, never to --force.
struct AllowDeletes {
    int count = 0;
    int cloudCount = 0, notebookCount = 0;  // per side, as in BlockedDeletes
    int total = 0;                          // the guard's total, "11 of 21"
    QDateTime at;                           // UTC
    QDateTime runStart;                     // the stopped run's start (BlockedDeletes.runStart)
};

QString allowDeletesPath(const QString &appState);
bool writeAllowDeletes(const QString &appState, const AllowDeletes &allow);
// Read and removed in one go, so it serves one run. A mark older than 30
// minutes (its run never started: a pause set in between) or one that
// cannot be read is removed, and nothing is returned.
std::optional<AllowDeletes> takeAllowDeletes(const QString &appState, const QDateTime &now);

// The smallest --max-delete that lets largestSide deletions out of total
// through: ceil(100 * largestSide / total), as rclone 1.75.2 compares the
// real percentage. Never below configured, never above 100.
int maxDeletePercent(int largestSide, int total, int configured);
// The arguments with "--max-delete N" or "--max-delete=N" set to percent,
// or the flag added at the end.
QStringList withMaxDelete(const QStringList &arguments, int percent);
// The --max-delete in the arguments; bisync's default, 50, when there is none.
int configuredMaxDelete(const QStringList &arguments);

// "drivesync deletes --list | --allow <count> [--json]". --list prints what
// the newest run would have deleted (JSON: count, cloudCount,
// notebookCount, guardCount, guardTotal, runStart, and list, at most 500
// {path, deletedInCloud}, the ones gone from the cloud first). --allow takes
// the number of files in that list, both sides, writes the one-run mark and
// starts drive-sync.service; for a fix whose final run stopped at the guard,
// it sends the fix back to that run instead. Refused while paused, during a
// takeover or another fix, and when DriveSync does not run drive-sync.
int runDeletesCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);
