#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

// The bar asks for the state every 5 to 30 seconds and must not walk all of
// ~/Drive each time, so the last count of conflict copies is kept in a small
// file that the window's scans and the status command share.

constexpr int kConflictCacheSeconds = 120;

struct ConflictCount {
    int count = 0;
    QString error;        // "" | "noDrive" | "noFind" | "scanFailed"
    QDateTime scannedAt;  // invalid when there is no usable cache
};

// The find arguments every scan uses: conflict copies of files under drive,
// skipping folders that never sync.
QStringList conflictFindArguments(const QString &drive);
ConflictCount readConflictCache(const QString &file);
// Written beside the target and renamed, so a reader never sees half a file.
bool writeConflictCache(const QString &file, const ConflictCount &count);
// Fresh when scanned less than kConflictCacheSeconds ago, and not in the future.
bool conflictCacheFresh(const ConflictCount &count, const QDateTime &now);
