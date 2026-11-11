#pragma once

#include <QDateTime>
#include <QStringList>
#include <QTextStream>

#include "backend.h"
#include "syncconfig.h"

// The commands that change drive-sync, for the window, the bar widget and
// the packaged units. Each returns the exit code: 0 done, 1 refused or
// failed, with the reason on err. With "--json" the answer goes to out as
// {"ok": bool, "error": text} instead.

// Runs a program and waits up to timeoutMs, killing it after that. output
// gets stdout when given; error gets stderr, or why it did not run.
bool runProcess(const QString &program, const QStringList &arguments, int timeoutMs, QByteArray *output,
                QString *error);

// The lock file a takeover holds while it runs, and whether it is still held:
// the pid in it must be alive and be drivesync, so a takeover killed halfway
// never blocks "sync now" for good.
QString takeoverLockPath(const QString &appState);
bool takeoverLockHeld(const QString &appState, const QString &proc);

// "drivesync sync": one normal run now, queued without waiting for it.
int runSyncCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);

// "drivesync pause [--minutes N | --until-tomorrow]": no flag pauses until
// "drivesync resume". Refused unless DriveSync runs drive-sync, because the
// old units never look at the mark.
int runPauseCommand(const QStringList &arguments, const BackendPaths &paths, const QDateTime &now, QTextStream &out,
                    QTextStream &err);

// "drivesync resume": clears the mark and starts one run.
int runResumeCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);

// "drivesync run-condition", the ExecCondition of drive-sync.service: 0 runs,
// 1 skips, 255 fails (runOnlyWhen crashed or exited 255, as systemd reads it). runOnlyWhen goes first, so a tick that could not run anyway does
// not use up the end of a pause; then a pause that ended is cleared and runs.
int runConditionCommand(const BackendPaths &paths, const SyncConfig &config, const QDateTime &now);

// What "drivesync run" hands to rclone, or nothing with a refusal when the
// filters check fails (see filtersUsable).
QStringList runCommandPlan(const SyncConfig &config, QString *refusal);

// "drivesync run", the ExecStart of drive-sync.service. On success it never
// returns: rclone replaces the process, so systemd sees rclone's exit code
// and signals. A refusal is printed and written to the log, and returns 1.
int runRunCommand(const BackendPaths &paths, QTextStream &err);
