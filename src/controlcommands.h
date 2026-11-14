#pragma once

#include <QDateTime>
#include <QStringList>
#include <QTextStream>

#include <optional>

#include "backend.h"
#include "blockeddeletes.h"
#include "syncconfig.h"

// The commands that change drive-sync, for the window, the bar widget and
// the packaged units. Each returns the exit code: 0 done, 1 refused or
// failed, with the reason on err. With "--json" the answer goes to out as
// {"ok": bool, "error": text} instead.

// Answers the caller of a command once: {"ok", "error"} on out with json,
// else the error on err. Returns the exit code.
int commandAnswer(bool json, const QString &error, QTextStream &out, QTextStream &err);

// Runs a program and waits up to timeoutMs, killing it after that. output
// gets stdout when given; error gets stderr, or why it did not run.
bool runProcess(const QString &program, const QStringList &arguments, int timeoutMs, QByteArray *output,
                QString *error);

// The lock file a takeover holds while it runs, and whether it is still held:
// the pid in it must be alive and be drive-sync, so a takeover killed halfway
// never blocks "sync now" for good.
QString takeoverLockPath(const QString &appState);
bool takeoverLockHeld(const QString &appState, const QString &proc);

// "drive-sync sync": one normal run now, queued without waiting for it.
int runSyncCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);

// "drive-sync pause [--minutes N | --until-tomorrow]": no flag pauses until
// "drive-sync resume". Refused unless DriveSync runs drive-sync, because the
// old units never look at the mark.
int runPauseCommand(const QStringList &arguments, const BackendPaths &paths, const QDateTime &now, QTextStream &out,
                    QTextStream &err);

// "drive-sync resume": clears the mark and starts one run.
int runResumeCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);

// "drive-sync run-condition", the ExecCondition of drive-sync.service: 0 runs,
// 1 skips (always while a fix is under way), 255 fails (runOnlyWhen crashed or exited 255, as systemd reads it). runOnlyWhen goes first, so a tick that could not run anyway does
// not use up the end of a pause; then a pause that ended is cleared and runs.
int runConditionCommand(const BackendPaths &paths, const SyncConfig &config, const QDateTime &now);

// What "drive-sync run" hands to rclone, or nothing with a refusal when the
// filters check fails (see filtersUsable). With a confirmation of blocked
// deletions, --max-delete is raised just enough for them.
QStringList runCommandPlan(const SyncConfig &config, QString *refusal, const std::optional<AllowDeletes> &allow = {});

// The plan for the run that starts now: runCommandPlan, or, when the user
// confirmed blocked deletions (the mark, taken here so it serves one run),
// first a --dry-run with the raised guard into
// <appState>/allowed-dry-run.log. The raised guard goes to the real run only
// when that dry run ends well and deletes no more on either side than was
// confirmed: one --max-delete covers both sides, in whole percents. Else the
// config's own guard stops the run again and the log shows the list as it
// is now. *note gets the line for the log, empty when there was no mark.
QStringList runPlanTakingMark(const SyncConfig &config, const QString &appState, const QString &rclone,
                              const QDateTime &now, QString *refusal, QString *note);
// "DriveSync raised --max-delete to 53% for this run: 11 deletions confirmed".
QString allowedRunLogLine(const AllowDeletes &allow, int percent);
// One line in rclone's format ("2026/11/12 18:41:19 INFO  : message", local time).
void appendToLog(const QString &logFile, const QString &level, const QString &message);
// systemctl --user start --no-block drive-sync.service; systemctl's error, or empty.
QString startOneRun(const BackendPaths &paths);

// "drive-sync run", the ExecStart of drive-sync.service. On success it never
// returns: rclone replaces the process, so systemd sees rclone's exit code
// and signals. A refusal is printed and written to the log, and returns 1.
int runRunCommand(const BackendPaths &paths, QTextStream &err);
