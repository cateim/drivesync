#include "clockformat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

ClockHours hoursInFormat(const QString &format) {
    bool quoted = false;
    bool twelveHour = false;  // "h": 1 to 12 only next to an AM/PM marker
    bool anyHour = false;
    bool amPm = false;
    for (int i = 0; i < format.size(); ++i) {
        const QChar c = format.at(i);
        if (c == '\'') {
            if (i + 1 < format.size() && format.at(i + 1) == '\'')
                ++i;  // '' is a literal quote, inside quotes or out
            else
                quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (c == 'h') {
            twelveHour = true;
            anyHour = true;
        } else if (c == 'H') {
            anyHour = true;
        } else if (c == 'a' || c == 'A') {
            amPm = true;
        }
    }
    if (!anyHour)
        return ClockHours::Unknown;
    return amPm && twelveHour ? ClockHours::Twelve : ClockHours::TwentyFour;
}

namespace {

// The first object with the clock's id anywhere in the layout: sections are
// arrays of widget entries, and a group can hold more of them.
QJsonObject findClock(const QJsonValue &value) {
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.value("id").toString() == "omarchy.clock")
            return object;
        for (const QJsonValue &child : object) {
            const QJsonObject found = findClock(child);
            if (!found.isEmpty())
                return found;
        }
    } else if (value.isArray()) {
        for (const QJsonValue &child : value.toArray()) {
            const QJsonObject found = findClock(child);
            if (!found.isEmpty())
                return found;
        }
    }
    return {};
}

}  // namespace

ClockHours barClockHours(const QList<QByteArray> &shellJsons) {
    QString position;
    QJsonValue layout;
    bool haveLayout = false;
    for (const QByteArray &text : shellJsons) {
        const QJsonObject bar = QJsonDocument::fromJson(text).object().value("bar").toObject();
        if (position.isEmpty())
            position = bar.value("position").toString();
        if (!haveLayout && bar.contains("layout")) {
            layout = bar.value("layout");
            haveLayout = true;
        }
    }
    const QJsonObject clock = findClock(layout);
    if (clock.isEmpty())
        return ClockHours::Unknown;
    // A side bar shows the vertical format. Omarchy's own defaults, for an
    // entry that names no format, are both 24 hours.
    const bool vertical = position == "left" || position == "right";
    const QString format = vertical ? clock.value("verticalFormat").toString("HH mm")
                                    : clock.value("format").toString("dddd HH:mm");
    return hoursInFormat(format);
}

QString clockTimeFormat(ClockHours hours, const QLocale &fallback) {
    switch (hours) {
    case ClockHours::TwentyFour:
        return "HH:mm";
    case ClockHours::Twelve:
        return "h:mm AP";
    case ClockHours::Unknown:
        break;
    }
    return fallback.timeFormat(QLocale::ShortFormat);
}

ClockPaths ClockPaths::defaults() {
    const QString omarchy = qEnvironmentVariable("OMARCHY_PATH", "/usr/share/omarchy");
    return {{QDir::homePath() + "/.config/omarchy/shell.json", omarchy + "/config/omarchy/shell.json"},
            QLocale::system()};
}

QString currentClockTimeFormat(const ClockPaths &paths) {
    QList<QByteArray> texts;
    for (const QString &path : paths.shellJsons) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            texts << file.readAll();
    }
    return clockTimeFormat(barClockHours(texts), paths.fallback);
}

ClockFormat::ClockFormat(const ClockPaths &paths, QObject *parent) : QObject(parent), m_paths(paths) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &ClockFormat::reload);
    reload();
}

void ClockFormat::reload() {
    const auto watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);

    // The shell saves shell.json by replacing it, which drops it from the
    // watch; watch its directory too and re-arm every time. Only the user's
    // file can change while the window is open.
    if (!m_paths.shellJsons.isEmpty()) {
        const QString user = m_paths.shellJsons.first();
        const QString directory = QFileInfo(user).absolutePath();
        if (QFileInfo::exists(directory))
            m_watcher.addPath(directory);
        if (QFileInfo::exists(user))
            m_watcher.addPath(user);
    }

    const QString format = currentClockTimeFormat(m_paths);
    if (format == m_timeFormat)
        return;
    m_timeFormat = format;
    emit changed();
}
