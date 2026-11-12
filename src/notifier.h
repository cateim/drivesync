#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

#include "backend.h"
#include "syncconfig.h"

// The desktop notifications of drive-sync, ported from drive-sync-aviso:
// a critical card when a run fails, a reminder every hour while it keeps
// failing, and a normal card saying since when once a run works again. The
// watchdog covers what no run can report, because then no run happens: a
// timer that is off or not enabled, or a unit that does not load. It also
// reminds once a day of a pause with no end, so a forgotten pause cannot
// become the silent outage this app exists to prevent.
//
// Exists because on 2026-11-06 the bisync failed 26 times in a row over six
// hours with nothing on screen.

struct NotifierPaths {
    QString notifySend;
    QString gdbus;    // closes a card by id
    QString omarchy;  // "omarchy notification dismiss" closes a card already shown, by title
    QLocale locale;   // for the dates and the AM/PM text
    QString timeFormat;  // the bar clock's hours (clockformat.h); empty: the locale's own

    static NotifierPaths defaults(const QLocale &locale);
};

// "drivesync notify", the ExecStopPost of drive-sync.service, with systemd's
// $SERVICE_RESULT. Always returns 0: a notification that cannot be shown
// must not turn a good run into a failed unit.
int runNotifyCommand(const QString &serviceResult, const BackendPaths &paths, const NotifierPaths &tools,
                     const QDateTime &now);

// "drivesync watchdog", from drive-sync-watchdog.timer every hour. A config
// that cannot be read (configError) is the problem it reports.
int runWatchdogCommand(const BackendPaths &paths, const SyncConfig &config, const NotifierPaths &tools,
                       const QDateTime &now, const QString &configError = {});
