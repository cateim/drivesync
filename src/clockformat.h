#pragma once

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

// Clock times follow the clock on the Omarchy bar: 24 hours when it shows 24
// hours, AM/PM when it shows AM/PM. Its format lives in Omarchy's shell.json,
// and a right click on the clock cycles it, so the window watches the file.

enum class ClockHours { Unknown, Twelve, TwentyFour };

// What a Qt date-time format says about the hour: twelve with an AM/PM marker
// and an "h", twenty-four with any other hour, unknown with no hour at all.
// Quoted text is literal, so "'at' HH:mm" is twenty-four.
ClockHours hoursInFormat(const QString &format);

// The hours of the bar clock, from the text of shell.json files in order of
// precedence (the user's, then Omarchy's defaults): the bar position and the
// layout each come from the first file that has them. Unknown when no file
// has a layout or the layout has no clock.
ClockHours barClockHours(const QList<QByteArray> &shellJsons);

// The Qt format for a clock time: "HH:mm", "h:mm AP", or the fallback
// locale's own short time format when the bar says nothing.
QString clockTimeFormat(ClockHours hours, const QLocale &fallback);

struct ClockPaths {
    QStringList shellJsons;  // ~/.config/omarchy/shell.json, then Omarchy's defaults
    QLocale fallback;        // the system locale, for a bar without a clock

    static ClockPaths defaults();
};

// Reads the files once: for the headless commands, which do not watch.
QString currentClockTimeFormat(const ClockPaths &paths = ClockPaths::defaults());

class ClockFormat : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString timeFormat READ timeFormat NOTIFY changed)

public:
    explicit ClockFormat(const ClockPaths &paths = ClockPaths::defaults(), QObject *parent = nullptr);

    QString timeFormat() const { return m_timeFormat; }

signals:
    void changed();

private:
    void reload();

    ClockPaths m_paths;
    QString m_timeFormat;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
