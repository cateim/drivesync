#include "legacymigration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString renamed(const QString &value, const QList<LegacyFolder> &folders, bool *changed) {
    for (const LegacyFolder &folder : folders) {
        if (value == folder.from || value.startsWith(folder.from + "/")) {
            *changed = true;
            return folder.to + value.mid(folder.from.size());
        }
    }
    return value;
}

QJsonValue renamed(const QJsonValue &value, const QList<LegacyFolder> &folders, bool *changed) {
    if (value.isString())
        return renamed(value.toString(), folders, changed);
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue &item : value.toArray())
            array.append(renamed(item, folders, changed));
        return array;
    }
    if (value.isObject()) {
        QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            it.value() = renamed(it.value(), folders, changed);
        return object;
    }
    return value;
}

// Another drive-sync process (the bar polls every few seconds while a timer
// tick starts) may move the same thing first: a source that is gone after a
// failed rename is that, not a problem.
bool moveOrGone(const QString &from, const QString &to) {
    return QDir().rename(from, to) || !QFileInfo::exists(from);
}

QStringList mergeInto(const QString &from, const QString &to) {
    QStringList left;
    const QDir source(from);
    const auto entries = source.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        const QString target = to + "/" + name;
        if (QFileInfo::exists(target))
            left << QString("%1/%2 stays: %3 already exists").arg(from, name, target);
        else if (!moveOrGone(from + "/" + name, target))
            left << QString("cannot move %1/%2 to %3").arg(from, name, target);
    }
    // Empty only when everything moved; rmdir refuses anything else.
    QDir().rmdir(from);
    return left;
}

}  // namespace

QList<LegacyFolder> legacyFolders(const QString &home) {
    struct Target {
        const char *base;
        const char *name;
    };
    const Target targets[] = {{"/.config", "drive-sync"},
                              {"/.local/state", "drive-sync-app"},
                              {"/.local/share", "drive-sync"}};
    QList<LegacyFolder> folders;
    for (const char *oldName : {"drivesync", "syncwatch"})
        for (const Target &target : targets)
            folders.append({home + target.base + "/" + oldName, home + target.base + "/" + target.name});
    return folders;
}

QStringList moveLegacyFolders(const QList<LegacyFolder> &folders) {
    QStringList left;
    for (const LegacyFolder &folder : folders) {
        if (!QFileInfo(folder.from).isDir())
            continue;
        if (!QFileInfo::exists(folder.to)) {
            if (!QDir().mkpath(QFileInfo(folder.to).absolutePath())) {
                left << "cannot create the folder of " + folder.to;
                continue;
            }
            if (moveOrGone(folder.from, folder.to))
                continue;
        }
        left << mergeInto(folder.from, folder.to);
    }
    return left;
}

QStringList rewriteLegacyPaths(const QList<LegacyFolder> &folders, const QStringList &jsonFiles) {
    QStringList problems;
    for (const QString &path : jsonFiles) {
        QFile file(path);
        if (!file.exists())
            continue;
        if (!file.open(QIODevice::ReadOnly)) {
            problems << "cannot read " + path;
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (!document.isObject() && !document.isArray()) {
            problems << path + " is not JSON";
            continue;
        }
        bool changed = false;
        const QJsonValue value = renamed(document.isObject() ? QJsonValue(document.object())
                                                             : QJsonValue(document.array()),
                                         folders, &changed);
        if (!changed)
            continue;
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            problems << "cannot write " + path;
            continue;
        }
        out.write((value.isObject() ? QJsonDocument(value.toObject()) : QJsonDocument(value.toArray()))
                      .toJson(QJsonDocument::Indented));
        if (!out.commit())
            problems << "cannot write " + path;
    }
    return problems;
}

QStringList migrateFromLegacyNames(const QString &home) {
    const QList<LegacyFolder> folders = legacyFolders(home);
    QStringList problems = moveLegacyFolders(folders);
    // The config names its filters; each manifest names where the takeover put
    // the old files, which is where an undo looks for them.
    QStringList jsonFiles{home + "/.config/drive-sync/config.json"};
    const QString backupRoot = home + "/.local/share/drive-sync/taken-over";
    for (const QString &backup : QDir(backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        jsonFiles << backupRoot + "/" + backup + "/manifest.json";
    problems << rewriteLegacyPaths(folders, jsonFiles);
    return problems;
}
