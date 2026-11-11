#pragma once

#include <QDateTime>
#include <QStringList>
#include <QTextStream>

#include "backend.h"
#include "snapshot.h"

// "drivesync status": the state of drive-sync without a window, for the bar
// widget and scripts. It reads the same sources as the window, but waits for
// each answer instead of watching, and decides with the same buildSnapshot().

// The log, systemd and the conflict count, read once. A systemctl that fails
// or takes longer than timeoutMs counts as not working; nextRun gets the
// timer's next elapse, or stays invalid.
SnapshotSources gatherSources(const BackendPaths &paths, const QDateTime &now, int timeoutMs, QDateTime *nextRun);

// arguments are the ones after "status". Returns the exit code: 0 with the
// state printed (one English line, or JSON with --json), 1 with the usage on
// err for anything it does not know.
int runStatusCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err,
                     int timeoutMs = 5000);

QString usageText();
