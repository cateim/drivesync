#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <functional>

#include "backend.h"
#include "fixstate.h"
#include "notifier.h"
#include "syncconfig.h"

// "drive-sync fix": rebuilds the bisync base the way drive-sync.sh's resync
// did, with ~/Drive still in use (spec section 5). See fixstate.h for the
// steps.

// "bisync", remote, local, --workdir, --filters-file, the config's flags
// minus the routine ones (--resilient, --recover, --max-delete,
// --conflict-resolve, --check-sync, and its own --max-lock, --log-file,
// --log-level and -v), the --fast-list rule of rcloneRunArguments, then
// --resync --resync-mode newer --check-sync=false --max-lock 2m -v
// --log-file <logFile> [--dry-run].
QStringList fixResyncArguments(const SyncConfig &config, bool dryRun, const QString &logFile);
// "gdrive:" + "a/b.txt" -> "gdrive:a/b.txt"; "gdrive:sub" -> "gdrive:sub/a/b.txt".
QString remoteFile(const QString &remote, const QString &path);
struct DryRunCopy {
    QString path;
    bool toNotebook = false;  // from the cloud to ~/Drive; else the other way
};
// The "Skipped copy as --dry-run" lines of a resync dry run, each with the
// direction of the "Resync is copying files to" heading above it (rclone
// 1.75.2). The copies to the notebook come first, since a file deleted there
// and coming back is among them, then the ones to the cloud, at most limit
// in all. *total and *toNotebook count every copy.
QList<DryRunCopy> dryRunCopies(const QByteArray &log, int limit, int *total, int *toNotebook);
// Why a fix cannot start with this config, or empty.
QString fixConfigRefusal(const SyncConfig &config);

// What the unit runs, and how it waits. Tests replace both.
struct FixTools {
    QString rclone;
    std::function<void(int seconds)> wait;  // sleeps in production

    static FixTools defaults();
};

// "drive-sync fix --start | --confirm | --answers <file.json> | --cancel |
// --retry [--json]". --answers takes {"<path>": "cloud" | "notebook"}, and
// every file the fix asked about must have one. systemctl is the one in paths.
int runFixCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);
// --retry: from a failed step, or one the unit left halfway; starts the
// unit. Call it holding the FixLock. The error, or empty.
QString retryFix(const BackendPaths &paths, const FixState &state);

// "drive-sync fix-run", the ExecStart of drive-sync-fix.service: runs the
// current step and the ones after it until the fix needs the user, fails or
// ends. Returns 1 when a step failed. A config that cannot be read
// (configError) fails the step.
int runFixUnit(const BackendPaths &paths, const SyncConfig &config, const FixTools &tools,
               const NotifierPaths &notify, const QString &configError = {});
