#include "snapshot.h"

#include <QJsonArray>
#include <QTimeZone>
#include <algorithm>

#include "backend.h"

Snapshot buildSnapshot(const SnapshotSources &src) {
    Snapshot s;
    const QDateTime &now = src.now;
    const QString active = src.service.value("ActiveState");
    const bool serviceBusy = active == "activating" || active == "deactivating" || active == "active";

    // The newest run is still going when systemd says so, when a resync owns
    // the log, or when rclone wrote to it a moment ago (systemd is polled,
    // the log is watched, so the log can know first).
    const SyncRun *open = !src.runs.isEmpty() && src.runs.last().outcome == "open" ? &src.runs.last() : nullptr;
    const bool running = open && (serviceBusy || src.resyncRunning || open->lastSeen.secsTo(now) < kAliveSeconds);

    const SyncRun *lastFinished = nullptr;
    QString lastFinishedOutcome;
    QDateTime failingSince;
    bool successSeen = false;
    QList<qint64> durations;
    for (auto it = src.runs.crbegin(); it != src.runs.crend(); ++it) {
        if (running && &*it == open)
            continue;
        const QString outcome = it->outcome == "open" ? QStringLiteral("interrupted") : it->outcome;
        if (s.runs.size() < kMaxRuns)
            s.runs.append(Backend::runToMap(*it, outcome));
        if (!lastFinished) {
            lastFinished = &*it;
            lastFinishedOutcome = outcome;
        }
        if (outcome == "success") {
            if (!s.lastSuccess.isValid())
                s.lastSuccess = it->end;
            successSeen = true;
            if (!it->resync && durations.size() < 10)
                durations.append(it->start.secsTo(it->end));
        } else if (!successSeen) {
            // Walking newest to oldest, the last run seen before a success is
            // the one where the current trouble began.
            failingSince = it->start;
        }
    }

    SyncInputs in;
    in.systemctlWorks = src.systemctlWorks;
    in.logFound = src.logFound;
    in.driveFound = src.driveFound;
    in.serviceLoad = src.service.value("LoadState");
    in.serviceActive = active;
    in.serviceResult = src.service.value("Result");
    in.serviceStart = fromUnixStamp(src.service.value("ExecMainStartTimestamp"));
    in.timerActive = src.timer.value("ActiveState");
    in.timerFileState = src.timer.value("UnitFileState");
    in.resyncRunning = src.resyncRunning;
    // The old units never look at the mark, so it only counts once DriveSync runs drive-sync.
    const bool paused = src.pause.present && src.managed;
    in.paused = paused;
    in.running = running;
    if (lastFinished) {
        in.lastOutcome = lastFinishedOutcome;
        in.lastStart = lastFinished->start;
        in.lastCauseKind = lastFinished->causeKind;
        in.lastCauseDetail = lastFinished->causeDetail;
    }
    in.lastSuccess = s.lastSuccess;
    in.now = now;
    in.conflicts = src.conflicts;
    if (src.fixPresent) {
        in.fixStep = src.fix.step;
        in.fixError = src.fix.error;
        const bool starting = src.fix.stepStarted.isValid() && src.fix.stepStarted.secsTo(now) < kFixStartGraceSeconds;
        in.fixInterrupted = fixWorking(src.fix.step) && !src.fixUnitActive && !starting;
    }

    std::sort(durations.begin(), durations.end());
    s.typicalSeconds = durations.isEmpty() ? 0 : int(durations.at(durations.size() / 2));
    s.currentRun = running ? Backend::runToMap(*open, "running") : QVariantMap();
    s.summary = Backend::summarize(in);
    s.tone = toneFor(s.summary.status);
    s.running = running;
    // The bar shows a fix at work with the resync icon: it is one.
    s.resync = src.resyncRunning || s.summary.status == "fixing";
    s.conflicts = src.conflicts;
    s.managed = src.managed;
    s.paused = paused;
    s.pausedUntil = paused ? src.pause.until : QDateTime();
    // A run started while paused would only be skipped by run-condition; one
    // after the pause ended runs and clears the mark.
    s.canSyncNow = src.systemctlWorks && in.serviceLoad == "loaded" && !serviceBusy && !running && !src.resyncRunning
                   && !(paused && !pauseEnded(src.pause, now)) && !src.fixPresent;
    if (lastFinished) {
        s.lastRunStart = lastFinished->start;
        s.lastRunEnd = lastFinished->end;
        s.lastRunOutcome = lastFinishedOutcome;
    }
    // No failure in the log after its last success means only systemd saw
    // one: it began with the service's last start, not with that good run.
    if (s.summary.status == "failed")
        s.failingSince = failingSince.isValid() ? failingSince : in.serviceStart;
    return s;
}

namespace {

qint64 unixOrZero(const QDateTime &when) { return when.isValid() ? when.toSecsSinceEpoch() : 0; }

}  // namespace

QJsonObject snapshotToJson(const Snapshot &s, const QDateTime &nextRun, const QDateTime &now) {
    return {
        {"version", 2},
        {"now", unixOrZero(now)},
        {"status", s.summary.status},
        {"tone", s.tone},
        {"causeKind", s.summary.causeKind},
        {"causeDetail", s.summary.causeDetail},
        {"warnings", QJsonArray::fromStringList(s.summary.warnings)},
        {"running", s.running},
        {"resync", s.resync},
        {"canSyncNow", s.canSyncNow},
        {"lastSuccess", unixOrZero(s.lastSuccess)},
        {"lastRun", QJsonObject{{"start", unixOrZero(s.lastRunStart)},
                                {"end", unixOrZero(s.lastRunEnd)},
                                {"outcome", s.lastRunOutcome}}},
        {"currentRun", QJsonObject{{"start", unixOrZero(s.currentRun.value("start").toDateTime())},
                                   {"phase", s.currentRun.value("phase").toString()}}},
        {"nextRun", unixOrZero(nextRun)},
        {"failingSince", unixOrZero(s.failingSince)},
        {"typicalSeconds", s.typicalSeconds},
        {"conflicts", s.conflicts},
        {"managed", s.managed},
        {"paused", s.paused},
        {"pausedUntil", unixOrZero(s.pausedUntil)},
    };
}

QString toneFor(const QString &status) {
    static const QStringList errors{"failed", "timerOff", "unavailable", "notLoaded", "noLog", "noDrive", "fixFailed"};
    static const QStringList warnings{"interrupted", "skipped", "paused", "stale", "noRuns", "conflicts", "fixWaiting"};
    if (errors.contains(status))
        return "error";
    if (warnings.contains(status))
        return "warning";
    if (status == "running" || status == "resync" || status == "fixing")
        return "busy";
    // Only "ok" is fine: a status added without a tone shows up as an error
    // instead of a green cloud.
    return status == "ok" ? "ok" : "error";
}

QStringList systemdShowArguments() {
    return {"--user", "show", "--timestamp=unix", "drive-sync.service", "drive-sync.timer", "drive-sync-fix.service", "-p",
            "LoadState,ActiveState,Result,ExecMainStartTimestamp,UnitFileState"};
}

bool fixUnitBusy(const QHash<QString, QString> &unit) {
    // A oneshot unit is "activating" for as long as its ExecStart runs.
    const QString active = unit.value("ActiveState");
    return active == "activating" || active == "active" || active == "deactivating" || active == "reloading";
}

QStringList listTimersArguments() {
    return {"--user", "list-timers", "--all", "-o", "json", "drive-sync.timer"};
}

QDateTime fromUnixStamp(const QString &value) {
    if (!value.startsWith('@'))
        return {};
    bool ok = false;
    const qint64 seconds = value.mid(1).toLongLong(&ok);
    return ok && seconds > 0 ? QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC) : QDateTime();
}
