#include "notifier.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

#include "controlcommands.h"
#include "pausestate.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kToolTimeoutMs = 10000;
constexpr int kRepeatSeconds = 3600;
constexpr int kPauseReminderSeconds = 86400;

QString tr(const char *text) { return QCoreApplication::translate("Notifier", text); }

// One open problem: when it began, when it was last shown, and the card's id.
struct Problem {
    QDateTime since;
    QDateTime last;
    int id = 0;
};

Problem readProblem(const QString &file) {
    Problem p;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return p;
    const QJsonObject o = QJsonDocument::fromJson(in.readAll()).object();
    auto when = [](qint64 s) { return s > 0 ? QDateTime::fromSecsSinceEpoch(s, QTimeZone::UTC) : QDateTime(); };
    p.since = when(o.value("since").toInteger());
    p.last = when(o.value("last").toInteger());
    p.id = qMax(0, o.value("id").toInt());
    return p;
}

void writeProblem(const QString &file, const Problem &p) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return;
    auto seconds = [](const QDateTime &t) { return t.isValid() ? t.toSecsSinceEpoch() : 0; };
    out.write(QJsonDocument(QJsonObject{{"since", seconds(p.since)}, {"last", seconds(p.last)}, {"id", p.id}})
                  .toJson(QJsonDocument::Compact));
    out.commit();
}

QString at(const NotifierPaths &tools, const QDateTime &when) {
    return tools.locale.toString(when.toLocalTime(), QLocale::ShortFormat);
}

// The card's id, or 0 when there is no graphical session to show it.
int notify(const NotifierPaths &tools, const QString &urgency, const QString &icon, const QString &title,
           const QString &body) {
    QByteArray output;
    runProcess(tools.notifySend, {"-p", "-a", "syncwatch", "-u", urgency, "-i", icon, title, body}, kToolTimeoutMs,
               &output, nullptr);
    return QString::fromUtf8(output).trimmed().toInt();
}

// CloseNotification only removes a card that has not shown yet: the Omarchy
// shell keeps visible ones, so the shell's own command closes them by title.
void close(const NotifierPaths &tools, int id, const QStringList &titles) {
    if (id > 0)
        runProcess(tools.gdbus,
                   {"call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path",
                    "/org/freedesktop/Notifications", "--method", "org.freedesktop.Notifications.CloseNotification",
                    QString::number(id)},
                   kToolTimeoutMs, nullptr, nullptr);
    for (const QString &title : titles)
        runProcess(tools.omarchy, {"notification", "dismiss", title}, kToolTimeoutMs, nullptr, nullptr);
}

// An ongoing problem: shown the first time, then every hour.
void reportProblem(const NotifierPaths &tools, const QString &file, const QString &firstTitle,
                   const QString &reminderTitle, const QString &text, const QString &footer, const QDateTime &now) {
    Problem p = readProblem(file);
    QString title;
    if (!p.since.isValid()) {
        p.since = now;
        title = firstTitle;
    } else if (!p.last.isValid() || p.last.secsTo(now) >= kRepeatSeconds) {
        // No card ever reached the screen (last unset): try the first one again.
        title = p.last.isValid() ? reminderTitle : firstTitle;
    } else {
        return;
    }
    // Also the first time: an old card of the same problem does not stay stacked.
    close(tools, p.id, {firstTitle, reminderTitle});
    const int id = notify(tools, "critical", "dialog-warning", title, text + "\n" + footer.arg(at(tools, p.since)));
    p.last = id > 0 ? now : QDateTime();
    p.id = id;
    writeProblem(file, p);
}

// A problem that ended: its card goes, and a normal one says since when it lasted.
void reportSolved(const NotifierPaths &tools, const QString &file, const QString &title, const QString &text,
                  const QStringList &problemTitles) {
    const Problem p = readProblem(file);
    if (!p.since.isValid())
        return;
    close(tools, p.id, problemTitles);
    notify(tools, "normal", "dialog-information", title, text.arg(at(tools, p.since)));
    QFile::remove(file);
}

QString causeSentence(const QString &kind, const QString &detail) {
    if (kind == "lostListings")
        return tr("bisync lost its listings, the base of both sides. "
                  "To recover: syncwatch takeover --undo, then drive-sync.sh resync");
    if (kind == "outOfSync")
        return tr("The listings of the two sides don't match, so bisync dropped its base. "
                  "To recover: syncwatch takeover --undo, then drive-sync.sh resync");
    return detail;
}

// One line on what broke in the run that just ended. Only runs that started
// with it count, as in drive-sync-aviso; the log parser already turns an
// error before "Synching Path1" (rclone that did not start, or SyncWatch's
// refusal) into a failed run of its own.
QString cause(const QString &serviceResult, const BackendPaths &paths) {
    if (serviceResult == "timeout")
        return tr("The run went past its time limit (TimeoutStartSec) and was stopped.");
    QByteArray stamp;
    runProcess(paths.systemctl,
               {"--user", "show", "--timestamp=unix", "-p", "ExecMainStartTimestamp", "--value", "drive-sync.service"},
               kToolTimeoutMs, &stamp, nullptr);
    const QDateTime started = fromUnixStamp(QString::fromUtf8(stamp).trimmed());
    bool truncated = false;
    const QString text = SyncLog::readTail(paths.log, 256 * 1024, nullptr, &truncated);
    const QList<SyncRun> runs = SyncLog::parse(text, 5, truncated);
    // The log keeps whole seconds; systemd's start can be up to a second later.
    if (!runs.isEmpty() && (!started.isValid() || runs.last().start >= started.addSecs(-2))
        && runs.last().outcome == "failed") {
        const QString sentence = causeSentence(runs.last().causeKind, runs.last().causeDetail);
        if (!sentence.isEmpty())
            return sentence.left(200);
    }
    return tr("No detail in the log (result: %1).").arg(serviceResult.isEmpty() ? "?" : serviceResult);
}

QString showValue(const BackendPaths &paths, const QStringList &arguments) {
    QByteArray output;
    runProcess(paths.systemctl, arguments, kToolTimeoutMs, &output, nullptr);
    return QString::fromUtf8(output).trimmed();
}

// What keeps every run from happening; empty when all is well.
QString timerProblem(const BackendPaths &paths) {
    const QString load = showValue(paths, {"--user", "show", "-p", "LoadState", "--value", "drive-sync.service"});
    if (load != "loaded")
        return tr("drive-sync.service does not load (LoadState=%1). See: systemctl --user status drive-sync.service")
            .arg(load.isEmpty() ? "?" : load);
    // drive-sync.sh stops the timer on purpose during its resync.
    if (Backend::resyncRunning(paths.stateDir, paths.proc))
        return {};
    const QString active = showValue(paths, {"--user", "is-active", "drive-sync.timer"});
    if (active != "active")
        return tr("drive-sync.timer is %1: no run happens. To turn it back on: "
                  "systemctl --user enable --now drive-sync.timer")
            .arg(active.isEmpty() ? "?" : active);
    const QString enabled = showValue(paths, {"--user", "is-enabled", "drive-sync.timer"});
    if (enabled != "enabled")
        return tr("drive-sync.timer is %1: it runs now, but will not start on the next boot. "
                  "To fix: systemctl --user enable drive-sync.timer")
            .arg(enabled.isEmpty() ? "?" : enabled);
    return {};
}

void remindOfPause(const BackendPaths &paths, const NotifierPaths &tools, const QDateTime &now) {
    const QString file = pauseFilePath(paths.appState);
    PauseMark mark = readPause(file);
    if (!mark.present || mark.until.isValid())
        return;  // a pause with an end ends by itself
    const QDateTime since = mark.since.isValid() ? mark.since : now;
    const QDateTime last = mark.lastReminder.isValid() ? mark.lastReminder : since;
    if (last.secsTo(now) < kPauseReminderSeconds)
        return;
    notify(tools, "normal", "dialog-information", tr("SyncWatch is still paused"),
           tr("Drive has been paused since %1. Resume it in SyncWatch or with: syncwatch resume")
               .arg(at(tools, since)));
    // notify-send can take seconds: a resume or a new pause in the meantime wins.
    const PauseMark current = readPause(file);
    if (!current.present || current.since != mark.since || current.until != mark.until)
        return;
    mark.lastReminder = now;
    writePause(file, mark);
}

}  // namespace

NotifierPaths NotifierPaths::defaults(const QLocale &locale) {
    return {QStandardPaths::findExecutable("notify-send"), QStandardPaths::findExecutable("gdbus"),
            QStandardPaths::findExecutable("omarchy"), locale};
}

int runNotifyCommand(const QString &serviceResult, const BackendPaths &paths, const NotifierPaths &tools,
                     const QDateTime &now) {
    const QString file = paths.appState + "/notify-run.json";
    const QString stopped = tr("Drive stopped syncing");
    const QString stillStopped = tr("Drive is still stopped");
    if (serviceResult == "exec-condition")
        return 0;  // skipped on purpose: paused, or runOnlyWhen said no
    if (serviceResult == "success") {
        reportSolved(tools, file, tr("Drive is syncing again"), tr("It had stopped at %1."), {stopped, stillStopped});
        return 0;
    }
    reportProblem(tools, file, stopped, stillStopped, cause(serviceResult, paths),
                  tr("Stopped since %1. Log: %2").arg("%1", paths.log), now);
    return 0;
}

int runWatchdogCommand(const BackendPaths &paths, const SyncConfig &config, const NotifierPaths &tools,
                       const QDateTime &now, const QString &configError) {
    // Where the runs are skipped on purpose (another home mounted, say), the
    // watchdog has nothing to say either.
    if (!config.runOnlyWhen.isEmpty()
        && !runProcess(QStandardPaths::findExecutable("sh"), {"-c", config.runOnlyWhen}, kToolTimeoutMs, nullptr, nullptr))
        return 0;
    const QString file = paths.appState + "/notify-watchdog.json";
    const QString noSync = tr("Drive will not sync");
    const QString stillNoSync = tr("Drive is still not syncing");
    const QString problem = configError.isEmpty()
        ? timerProblem(paths)
        : tr("SyncWatch's config cannot be read, so no run happens: %1").arg(configError);
    if (!problem.isEmpty())
        reportProblem(tools, file, noSync, stillNoSync, problem, tr("Noticed at %1."), now);
    else
        reportSolved(tools, file, tr("Drive timer is back"), tr("The problem was noticed at %1."), {noSync, stillNoSync});
    remindOfPause(paths, tools, now);
    return 0;
}
