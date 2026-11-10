import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// The headline: how drive-sync is doing right now, and what to do about it.
Rectangle {
    id: panel

    property double now
    readonly property string status: backend.status
    readonly property var current: backend.currentRun
    readonly property bool busy: status === "running" || status === "resync"
    readonly property bool hasCurrent: !!current && current.start !== undefined
    readonly property real elapsed: hasCurrent ? Math.max(0, (now - current.start.getTime()) / 1000) : 0
    // The grouping comes from the backend, so the window, the bar and the
    // status command can never color the same state differently.
    readonly property color tone: backend.tone === "error" ? "#ff6b6b"
                                : backend.tone === "warning" ? "#ffb74d"
                                : theme.accent
    // Material Design Icons from the Nerd Font, the same glyphs the bar uses.
    readonly property string glyph: String.fromCodePoint(backend.tone === "ok" ? 0xF0160      // cloud-check
                                                       : backend.tone === "busy" ? 0xF063F    // cloud-sync
                                                       : status === "paused" ? 0xF18F6        // weather-cloudy-clock
                                                       : status === "timerOff" ? 0xF0164      // cloud-off-outline
                                                       : 0xF09E0)                             // cloud-alert

    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    border.width: 1
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.45)
    implicitHeight: content.implicitHeight + 36

    function lastSuccessRun() {
        for (let i = 0; i < backend.runs.length; ++i)
            if (backend.runs[i].outcome === "success")
                return backend.runs[i]
        return null
    }

    function headline() {
        translator.language
        switch (status) {
        case "ok": return qsTr("In sync")
        case "running": return qsTr("Syncing now")
        case "resync": return qsTr("Resync in progress")
        case "paused": return qsTr("Paused")
        case "failed": return qsTr("Sync failed")
        case "interrupted": return qsTr("The last run was cut off")
        case "skipped": return qsTr("Runs are being skipped")
        case "timerOff": return qsTr("drive-sync won't run")
        case "stale": return qsTr("No sync for a while")
        case "noDrive": return qsTr("The Drive folder isn't there")
        case "conflicts": return qsTr("%n conflict copies to sort out", "", backend.conflicts.length)
        case "unavailable": return qsTr("Can't ask systemd")
        case "notLoaded": return qsTr("drive-sync.service doesn't load")
        case "noLog": return qsTr("No drive-sync log")
        }
        return qsTr("Reading drive-sync…")
    }

    function phaseText(phase) {
        switch (phase) {
        case "listing": return qsTr("Listing both sides")
        case "comparing": return qsTr("Comparing the two sides")
        case "applying": return qsTr("Copying changes")
        case "updating": return qsTr("Saving the new listings")
        }
        return qsTr("Working")
    }

    function causeText(kind, detail) {
        switch (kind) {
        // Until SyncWatch can resync (step 3), a managed setup goes back to the old scripts for it.
        case "lostListings": return backend.managed
            ? qsTr("bisync lost its listings, the base of both sides. To recover: syncwatch takeover --undo, then drive-sync.sh resync")
            : qsTr("bisync lost its listings, the base of both sides. To recover: drive-sync.sh resync")
        case "outOfSync": return backend.managed
            ? qsTr("The listings of the two sides don't match, so bisync dropped its base. To recover: syncwatch takeover --undo, then drive-sync.sh resync")
            : qsTr("The listings of the two sides don't match, so bisync dropped its base. To recover: drive-sync.sh resync")
        case "timeout": return qsTr("The run went past its time limit (TimeoutStartSec) and systemd stopped it.")
        case "systemd": return qsTr("systemd says the last run failed (%1), and the log has nothing about it.").arg(detail)
        case "error": return detail
        }
        return qsTr("The log has no detail about it.")
    }

    function lastSyncLine() {
        const run = lastSuccessRun()
        if (!run || !Format.valid(run.end))
            return ""
        return qsTr("Last sync at %1 (%2), took %3.")
            .arg(Format.when(run.end, now)).arg(Format.ago(run.end, now)).arg(Format.duration(run.seconds))
    }

    function nextRunLine() {
        const next = backend.nextRun
        if (!Format.valid(next))
            return ""
        const minutes = Math.ceil((next.getTime() - now) / 60000)
        if (minutes <= 0)
            return qsTr("Next run any moment now.")
        if (minutes < 60)
            return qsTr("Next run in %n min.", "", minutes)
        return qsTr("Next run at %1.").arg(Format.when(next, now))
    }

    function progressLine() {
        if (!hasCurrent)
            return ""
        let line = qsTr("%1. Started at %2, %3 so far.")
            .arg(phaseText(current.phase)).arg(Format.clock(current.start)).arg(Format.duration(elapsed))
        if (current.listed > 0)
            line += " " + qsTr("%Ln files listed.", "", current.listed)
        return line
    }

    function pauseLine() {
        const until = backend.pausedUntil
        if (!Format.valid(until))
            return qsTr("Until you resume (U).")
        if (until.getTime() <= now)
            return qsTr("Resumes on the next run.")
        return qsTr("Until %1 (U resumes now).").arg(Format.when(until, now))
    }

    function lastRunLine() {
        const run = backend.runs.length ? backend.runs[0] : null
        if (!run)
            return ""
        return qsTr("Last run at %1 (%2). Log: %3")
            .arg(Format.when(run.start, now)).arg(Format.ago(run.start, now)).arg(Format.home(backend.logPath))
    }

    function details() {
        translator.language
        const lines = []
        switch (status) {
        case "ok":
        case "conflicts":
        case "stale":
            lines.push(lastSyncLine(), nextRunLine())
            break
        case "running":
            lines.push(progressLine())
            break
        case "paused":
            lines.push(pauseLine(), lastSyncLine())
            break
        case "resync":
            lines.push(qsTr("drive-sync.sh resync stopped the timer on purpose; it comes back on when the resync ends."),
                       progressLine())
            break
        case "failed":
            lines.push(causeText(backend.causeKind, backend.causeDetail), lastRunLine())
            break
        case "interrupted":
            lines.push(qsTr("rclone stopped without an end line (killed, shut down or crashed). The next run recovers with --recover."),
                       lastRunLine(), nextRunLine())
            break
        case "skipped":
            lines.push(qsTr("systemd skips every run because /home isn't the Crucial one (emergency home?)."))
            break
        case "timerOff":
            lines.push(qsTr("No run happens until the timer is back on: systemctl --user enable --now drive-sync.timer"),
                       lastSyncLine())
            break
        case "noDrive":
            lines.push(qsTr("Expected at %1.").arg(Format.home(backend.drivePath)))
            break
        case "unavailable":
            lines.push(qsTr("systemctl didn't answer, so the state below comes from the log alone."))
            break
        case "notLoaded":
            lines.push(qsTr("LoadState is %1. See: systemctl --user status drive-sync.service").arg(backend.causeDetail))
            break
        case "noLog":
            lines.push(qsTr("Expected at %1.").arg(Format.home(backend.logPath)))
            break
        }
        return lines.filter(line => line !== "").join("\n")
    }

    function warningText(code) {
        translator.language
        switch (code) {
        case "timerOff": return qsTr("Timer is off")
        case "timerDisabled": return qsTr("Timer won't start on boot")
        case "conflicts": return qsTr("%n conflict copies", "", backend.conflicts.length)
        }
        return code
    }

    ColumnLayout {
        id: content
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 18 }
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                text: panel.glyph
                textFormat: Text.PlainText
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 30
                color: panel.tone
                SequentialAnimation on opacity {
                    running: panel.busy
                    loops: Animation.Infinite
                    onRunningChanged: if (!running) parent.opacity = 1
                    NumberAnimation { to: 0.25; duration: 700; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutQuad }
                }
            }

            Label {
                Layout.fillWidth: true
                text: panel.headline()
                font.pixelSize: 24
                font.weight: Font.DemiBold
                color: "#f4f4f5"
                elide: Text.ElideRight
            }
        }

        Label {
            Layout.fillWidth: true
            text: panel.details()
            visible: text !== ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            lineHeight: 1.25
            color: "#c4c4cc"
            font.pixelSize: 14
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: panel.busy
            indeterminate: !panel.hasCurrent || current.resync || backend.typicalSeconds <= 0
                           || panel.elapsed > backend.typicalSeconds
            value: backend.typicalSeconds > 0 ? Math.min(1, panel.elapsed / backend.typicalSeconds) : 0
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8
            visible: backend.warnings.length > 0

            Repeater {
                model: backend.warnings
                delegate: Rectangle {
                    required property string modelData
                    radius: Math.min(4, theme.radius)
                    color: "#2a2216"
                    border.color: "#6b5023"
                    implicitWidth: chip.implicitWidth + 16
                    implicitHeight: chip.implicitHeight + 8
                    Label {
                        id: chip
                        anchors.centerIn: parent
                        text: panel.warningText(parent.modelData)
                        color: "#ffcc80"
                        font.pixelSize: 13
                    }
                }
            }
        }
    }
}
