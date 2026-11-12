#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <optional>

// The bisync listings read back and compared, the way drive-sync.sh's
// conferir_base did after a resync. The base run lists both sides more than
// once, so a file that changed while it ran (an editor, an agent, a build)
// leaves the two listings different there; the next normal run would then
// stop with "out of sync" and lose the base. The fix settles each one first.

struct ListingEntry {
    qint64 size = -1;      // -1 when the listing does not know it
    qint64 mtimeNs = 0;    // Unix nanoseconds
    bool notUtf8 = false;  // the name is not UTF-8; it is then keyed by its quoted text
};

// Go's strconv.Unquote for the "%q" names bisync writes, without the outer
// quotes; ok is false on a malformed escape. A name whose bytes are not
// valid UTF-8 (only \x and octal escapes can make one) sets *notUtf8.
QString unquoteGo(const QString &quoted, bool *ok, bool *notUtf8);

// Files only ("-" lines): comments and folders are skipped. An error names
// the file and the line number.
std::optional<QHash<QString, ListingEntry>> readListing(const QString &path, QString *error);

struct Divergence {
    QString path;
    QString kind;   // "onlyCloud" | "onlyNotebook" | "differs" | "notUtf8"
    QString newer;  // "cloud" | "notebook"
    ListingEntry cloud, notebook;
};

// Sorted by path. Two entries are equal when either size is negative or the
// sizes match, and the times are less than 1 ms apart (Google Drive's
// precision, and what rclone's check-sync also calls equal).
QList<Divergence> divergences(const QHash<QString, ListingEntry> &cloud, const QHash<QString, ListingEntry> &notebook);

// The single "<prefix>.path1.lst" (the cloud) and ".path2.lst" (the disk) in
// the work dir, ignoring the -dry, -old, -err and -new copies; empty when
// there is not exactly one of each.
QPair<QString, QString> currentListings(const QString &workDir);
