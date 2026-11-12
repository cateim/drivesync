#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// A fix rebuilds the bisync base of both sides while ~/Drive stays in use.
// It is a state machine kept in <appState>/fix.json: "drivesync fix" moves
// it on the user's word, and drive-sync-fix.service ("drivesync fix-run")
// does the long steps. While the file exists, run-condition skips every
// timer tick, so nothing else starts rclone in between.

// A file newer or only in the cloud after the base run, which waits for
// the user: spec section 5, step 5.
struct FixQuestion {
    QString path;
    QString kind;  // "onlyCloud" | "differs"
    qint64 cloudSize = -1, notebookSize = -1, cloudTime = 0, notebookTime = 0;  // Unix seconds
    QString answer;  // "" | "cloud" | "notebook"
};

struct FixState {
    QString step;        // "preparing" | "confirm" | "base" | "questions" | "finishing" | "failed"; "" when no fix
    QString failedStep;  // the step to retry from when step == "failed"
    QDateTime started, stepStarted, lastReminder;  // UTC
    int copies = 0;      // from the dry run
    QString error;       // one line for the window and the card
    QList<FixQuestion> questions;
    QStringList touched;  // notebook-newer files touched after the base run
};

QString fixStatePath(const QString &appState);     // <appState>/fix.json
QString fixDryRunLogPath(const QString &appState);  // <appState>/fix-dry-run.log
bool fixPresent(const QString &appState);
// No file: an empty step. A file that cannot be read is a failed fix with
// that error: holding the timer is the safer mistake, and
// "drivesync fix --cancel" clears it.
FixState readFix(const QString &appState);
bool writeFix(const QString &appState, const FixState &state);
// Also removes the dry-run log and the window's answers file. True also when
// there was nothing to clear.
bool clearFix(const QString &appState);
// The steps that wait for the user, and the ones the unit works through.
bool fixWaiting(const QString &step);
bool fixWorking(const QString &step);
