#include "syncconfig.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr int kConfigVersion = 1;

bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}

}  // namespace

QString defaultSyncConfigPath() {
    return QDir::homePath() + "/.config/drivesync/config.json";
}

std::optional<SyncConfig> loadSyncConfig(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QString("cannot read %1").arg(path));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        fail(error, QString("%1 is not a JSON object: %2").arg(path, parseError.errorString()));
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    if (object.value("version").toInt() != kConfigVersion) {
        fail(error, QString("%1 has version %2, expected %3")
                        .arg(path, object.value("version").toVariant().toString())
                        .arg(kConfigVersion));
        return std::nullopt;
    }

    SyncConfig config;
    const struct {
        const char *key;
        QString *field;
    } required[] = {{"remote", &config.remote},
                    {"localPath", &config.localPath},
                    {"workDir", &config.workDir},
                    {"filtersFile", &config.filtersFile},
                    {"logFile", &config.logFile}};
    for (const auto &item : required) {
        *item.field = object.value(item.key).toString();
        if (item.field->isEmpty()) {
            fail(error, QString("%1 has no %2").arg(path, item.key));
            return std::nullopt;
        }
    }
    if (!object.value("rcloneFlags").isArray()) {
        fail(error, QString("%1 has no rcloneFlags").arg(path));
        return std::nullopt;
    }
    for (const QJsonValue &flag : object.value("rcloneFlags").toArray()) {
        if (!flag.isString()) {
            fail(error, QString("%1 has a flag that is not text").arg(path));
            return std::nullopt;
        }
        config.rcloneFlags << flag.toString();
    }
    config.runOnlyWhen = object.value("runOnlyWhen").toString();
    return config;
}

bool saveSyncConfig(const QString &path, const SyncConfig &config, QString *error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QString("cannot create the folder of %1").arg(path));
    const QJsonObject object{{"version", kConfigVersion},
                             {"remote", config.remote},
                             {"localPath", config.localPath},
                             {"workDir", config.workDir},
                             {"filtersFile", config.filtersFile},
                             {"logFile", config.logFile},
                             {"rcloneFlags", QJsonArray::fromStringList(config.rcloneFlags)},
                             {"runOnlyWhen", config.runOnlyWhen}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QString("cannot write %1").arg(path));
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(error, QString("cannot write %1").arg(path));
    return true;
}

QStringList rcloneRunArguments(const SyncConfig &config) {
    QStringList flags = config.rcloneFlags;
    // bisync's docs say it lists with --fast-list by default, but against
    // Google Drive on rclone 1.75 the listing took 476 s without the flag
    // and 40 s with it (2026-11-10, 33k files, same listing either way).
    // A config that turns ListR off on purpose keeps it off.
    if (!flags.contains("--fast-list") && !flags.join(' ').contains("ListR"))
        flags << "--fast-list";
    return QStringList{"bisync", config.remote, config.localPath, "--workdir", config.workDir,
                       "--filters-file", config.filtersFile}
           + flags + QStringList{"--log-file", config.logFile};
}

bool filtersUsable(const QString &filtersFile, QString *why) {
    QFile filters(filtersFile);
    if (!filters.open(QIODevice::ReadOnly))
        return fail(why, QString("the filters file %1 cannot be read").arg(filtersFile));
    QFile stored(filtersFile + ".md5");
    if (!stored.open(QIODevice::ReadOnly))
        return fail(why, QString("%1.md5 is missing, so bisync would lose its base").arg(filtersFile));
    const QByteArray actual = QCryptographicHash::hash(filters.readAll(), QCryptographicHash::Md5).toHex();
    if (stored.readAll().trimmed() != actual)
        return fail(why, QString("%1 changed since the last resync, so bisync would lose its base").arg(filtersFile));
    return true;
}
