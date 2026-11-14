#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// The app was SyncWatch until 2026-11-11 and "drivesync" until 2026-11-14 (the
// AUR has an unrelated drivesync, so it became drive-sync). Its folders under
// the home take the current name the first time any drive-sync command runs. A
// rename keeps every byte, so the filters and their ".md5" stay as bisync knows
// them, the takeover backups stay where their manifests say, and no resync is
// needed.
struct LegacyFolder {
    QString from;  // a folder under an earlier name
    QString to;    // the same folder under the current name
};

// ~/.config and ~/.local/share go to "drive-sync". ~/.local/state goes to
// "drive-sync-app", not "drive-sync": ~/.local/state/drive-sync is the old
// drive-sync.sh's (the resync pause mark), and a takeover moves that folder
// whole, so the app's own state must not live in it. The newer old name comes
// first, so when both exist it keeps its entries and the older one only gives
// what is missing.
QList<LegacyFolder> legacyFolders(const QString &home);

// A folder whose new name is free moves whole. When both exist, the old one
// gives the new one the entries it lacks; an entry both have stays where it
// is, and the old folder stays with it. Returns one line for each entry left
// behind; empty means nothing was.
QStringList moveLegacyFolders(const QList<LegacyFolder> &folders);

// Points every string in these JSON files that names a path inside an old
// folder at the new one. A file that names none is left untouched, so running
// it again changes nothing. Returns one line for each file it could not fix.
QStringList rewriteLegacyPaths(const QList<LegacyFolder> &folders, const QStringList &jsonFiles);

// Both steps, then the config and every takeover manifest. main() calls it
// before anything else, so nothing has created a new folder yet.
QStringList migrateFromLegacyNames(const QString &home);
