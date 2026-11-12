#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// What DriveSync runs when it owns drive-sync. A takeover copies every value
// byte for byte from the old unit: the bisync listings are named after the
// remote and local strings, so changing either one would ask for a resync.
struct SyncConfig {
    QString remote;           // "gdrive:", always Path1
    QString localPath;        // always Path2
    QString workDir;          // where bisync keeps its listings, never under ~/.cache
    QString filtersFile;      // with its ".md5" beside it
    QString logFile;
    QStringList rcloneFlags;  // every other rclone argument, in the old unit's order
    QString runOnlyWhen;      // a shell command; the run is skipped when it fails; empty runs always
};

// ~/.config/drivesync/config.json
QString defaultSyncConfigPath();
// Missing, unreadable, another version or a missing field: no config, and
// error says which.
std::optional<SyncConfig> loadSyncConfig(const QString &path, QString *error);
// Writes through QSaveFile, so a crash never leaves half a config behind.
bool saveSyncConfig(const QString &path, const SyncConfig &config, QString *error);
// The arguments after "rclone" for one normal run.
QStringList rcloneRunArguments(const SyncConfig &config);
// bisync keeps the md5 of the filters in "<file>.md5" and checks it on every
// run. A run that finds it missing or different stops with a critical error
// and renames the listings to "-err", and from then on only a resync brings
// the sync back (tried with rclone 1.75 on 2026-11-08). So nothing starts
// rclone unless this is true.
bool filtersUsable(const QString &filtersFile, QString *why);
// Ends every refusal of filtersUsable, so the log parser can tell a run
// refused for changed filters (which a fix repairs) from any other error.
inline constexpr char kFiltersChangedReason[] = "so bisync would lose its base";
