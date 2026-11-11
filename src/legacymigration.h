#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// SyncWatch was renamed DriveSync on 2026-11-11. Its folders under the home
// take the new name the first time any drivesync command runs. A rename keeps
// every byte, so the filters and their ".md5" stay as bisync knows them, the
// takeover backups stay where their manifests say, and no resync is needed.
struct LegacyFolder {
    QString from;  // the SyncWatch folder
    QString to;    // the DriveSync folder
};

// ~/.config, ~/.local/state and ~/.local/share: "syncwatch" to "drivesync".
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
QStringList migrateFromSyncWatch(const QString &home);
