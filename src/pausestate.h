#pragma once

#include <QDateTime>
#include <QString>

// A pause is a mark file, not a stopped timer. The timer keeps ticking and
// "drivesync run-condition" skips each run while the mark holds, so a pause
// survives a reboot and ends by itself at the first tick after its end, with
// no daemon waiting for it.
struct PauseMark {
    bool present = false;
    QDateTime since;         // UTC
    QDateTime until;         // UTC; invalid means until the user resumes
    QDateTime lastReminder;  // UTC; the daily reminder for a pause with no end
};

// <appState>/paused.json
QString pauseFilePath(const QString &appState);
// No file: no pause. A file that cannot be read counts as a pause with no
// end, because syncing while the user asked for a pause is the worse mistake.
PauseMark readPause(const QString &file);
bool writePause(const QString &file, const PauseMark &mark);
// True also when there was nothing to clear.
bool clearPause(const QString &file);
// A pause with an end that has come. A pause with no end never ends here.
bool pauseEnded(const PauseMark &mark, const QDateTime &now);
// "Until tomorrow": the next 08:00 in the zone of nowLocal, returned in UTC.
QDateTime nextMorning(const QDateTime &nowLocal);
