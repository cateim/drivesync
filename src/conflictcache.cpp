#include "conflictcache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimeZone>

QStringList conflictFindArguments(const QString &drive) {
    // bisync renames the losing side of a conflict to name.conflict1,
    // name.conflict2 and so on. Git, node_modules and virtualenvs never sync,
    // so they can't hold one and are skipped for speed.
    return {drive, "(", "-name", ".git", "-o", "-name", "node_modules", "-o", "-name", ".venv", ")",
            "-prune", "-o", "-type", "f", "-name", "*.conflict[0-9]*", "-printf", "%T@\\t%s\\t%P\\n"};
}

ConflictCount readConflictCache(const QString &file) {
    ConflictCount result;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return result;
    const QJsonObject json = QJsonDocument::fromJson(in.readAll()).object();
    const qint64 scanned = json.value("scannedAt").toInteger(0);
    if (scanned <= 0)
        return result;
    result.count = qMax(0, json.value("count").toInt());
    result.error = json.value("error").toString();
    result.scannedAt = QDateTime::fromSecsSinceEpoch(scanned, QTimeZone::UTC);
    return result;
}

bool writeConflictCache(const QString &file, const ConflictCount &count) {
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return false;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject json{{"scannedAt", count.scannedAt.isValid() ? count.scannedAt.toSecsSinceEpoch() : 0},
                           {"count", count.count},
                           {"error", count.error}};
    out.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    return out.commit();
}

bool conflictCacheFresh(const ConflictCount &count, const QDateTime &now) {
    if (!count.scannedAt.isValid())
        return false;
    const qint64 age = count.scannedAt.secsTo(now);
    return age >= 0 && age < kConflictCacheSeconds;
}
