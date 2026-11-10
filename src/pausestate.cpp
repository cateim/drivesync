#include "pausestate.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimeZone>

namespace {

QDateTime fromUnix(const QJsonValue &value) {
    const qint64 seconds = value.toInteger();
    return seconds > 0 ? QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC) : QDateTime();
}

qint64 toUnix(const QDateTime &when) { return when.isValid() ? when.toSecsSinceEpoch() : 0; }

}  // namespace

QString pauseFilePath(const QString &appState) { return appState + "/paused.json"; }

PauseMark readPause(const QString &file) {
    PauseMark mark;
    QFile in(file);
    if (!in.exists())
        return mark;
    mark.present = true;
    if (!in.open(QIODevice::ReadOnly))
        return mark;
    const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
    if (!document.isObject())
        return mark;
    const QJsonObject object = document.object();
    mark.since = fromUnix(object.value("since"));
    mark.until = fromUnix(object.value("until"));
    mark.lastReminder = fromUnix(object.value("lastReminder"));
    return mark;
}

bool writePause(const QString &file, const PauseMark &mark) {
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return false;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(QJsonObject{{"since", toUnix(mark.since)},
                                        {"until", toUnix(mark.until)},
                                        {"lastReminder", toUnix(mark.lastReminder)}})
                  .toJson(QJsonDocument::Compact));
    return out.commit();
}

bool clearPause(const QString &file) { return !QFile::exists(file) || QFile::remove(file); }

bool pauseEnded(const PauseMark &mark, const QDateTime &now) {
    return mark.present && mark.until.isValid() && mark.until <= now;
}

QDateTime nextMorning(const QDateTime &nowLocal) {
    // Local time here is fine: this runs once per pause, never in a loop.
    QDateTime morning(nowLocal.date(), QTime(8, 0), nowLocal.timeZone());
    if (morning <= nowLocal)
        morning = QDateTime(nowLocal.date().addDays(1), QTime(8, 0), nowLocal.timeZone());
    return morning.toUTC();
}
