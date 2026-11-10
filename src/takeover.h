#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "backend.h"
#include "snapshot.h"
#include "syncconfig.h"

// Taking over a drive-sync made by drive-sync.sh: its units, scripts and
// filters move aside, and SyncWatch's packaged units run the same rclone
// command. Nothing that names the bisync listings changes (remote, folder,
// work dir), and the filters move with their md5, so no resync is needed.

struct TakeoverPaths {
    QString userUnits;     // ~/.config/systemd/user
    QString binDir;        // ~/.local/bin
    QString oldStateDir;   // ~/.local/state/drive-sync
    QString appConfigDir;  // ~/.config/syncwatch
    QString backupRoot;    // ~/.local/share/syncwatch/taken-over
    QString systemctl;
    QString rclone;
    int serviceWaitMs = 20 * 60 * 1000;           // how long to wait for a run to end
    QString packagedUnits = "/usr/lib/systemd/user";
    QString appState;                             // holds the takeover lock and the pause mark
    QString systemUserUnits = "/etc/systemd/user";  // drop-ins there apply to user units too

    static TakeoverPaths defaults();
};

struct TakeoverPlan {
    QString state;           // "none" | "found" | "managed"
    SyncConfig config;       // what the takeover would write
    QString oldFiltersFile;  // the filters the old unit reads
    QStringList found;       // every old file that moves, for the window to show
    // Machine keys, translated in the window: "running", "lastRunNotSuccess",
    // "noListings", "filtersChanged", "remoteMissing", "unreadableUnit",
    // "unknownDirective:<key>", "differentValue:<key>", "targetExists",
    // "dropIn", "pathInsideMoved".
    QStringList blockers;

    bool canApply() const { return state == "found" && blockers.isEmpty(); }
};

// Reads the old setup and says what a takeover would do, or why it cannot.
// Anything in the old units that SyncWatch cannot express blocks it, so
// nothing is ever dropped silently.
TakeoverPlan detectTakeover(const TakeoverPaths &paths, const Snapshot &snapshot);

// Copies the filters and their md5 to a new path, refusing to overwrite, and
// checks that the copy is one bisync accepts.
bool copyFiltersWithMd5(const QString &from, const QString &to, QString *error);

// Takes over in this order, undoing every step done so far when one fails:
// note how the old timers were, stop them, wait for a run to end, copy the
// filters with their md5, write the config, disable the old timers, move the
// old units, scripts and state into <backupRoot>/<stamp>/ (manifest first),
// reload systemd, check that the packaged units are the ones systemd uses and
// carry no drop-in, wait for a run again, move the old filters last, enable
// the packaged timers. The old filters stay in place until no unit can read
// them any more, so a run started by hand halfway never finds them missing.
// A lock file makes "syncwatch sync" refuse while this runs. When a file
// cannot be moved back, the rollback stops there and leaves every timer off,
// because starting the old ones without their filters would cost a resync.
bool applyTakeover(const TakeoverPaths &paths, const TakeoverPlan &plan, const QDateTime &now, QString *error);

// Goes back to the newest takeover: refused while a run is going, when the
// filters changed since, or when anything already sits where an old file goes
// back. The packaged timers stop before the old ones return, so there is
// never a second scheduler; if a file cannot go back, the ones already back
// return to the backup and the packaged timers start again. Clears a pause,
// which the old units would not honour.
bool undoTakeover(const TakeoverPaths &paths, QString *error);

// "syncwatch takeover --check | --apply | --undo [--json]".
int runTakeoverCommand(const QStringList &arguments, const TakeoverPaths &paths, const BackendPaths &backend,
                       QTextStream &out, QTextStream &err);
