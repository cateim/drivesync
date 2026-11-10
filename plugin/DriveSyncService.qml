import QtQuick
import Quickshell
import Quickshell.Io
import "DriveSyncModel.js" as Model

// Polls `syncwatch status --json` and runs the widget's actions through
// `syncwatch` too (sync, pause, resume), so the widget never decides what a
// command may do: the app refuses what does not make sense.
Item {
  id: root

  property var settings: ({})

  readonly property string home: Quickshell.env("HOME") || ""
  readonly property string drivePath: home + "/Drive"
  readonly property string historyPath: localPath("open-history.sh")

  readonly property int refreshIntervalSec: Model.clampInt(setting("refreshIntervalSec", 30), 30, 10, 600)
  // While a run is going, poll fast so the end shows up within seconds.
  readonly property int pollMs: status.running || status.resync || starting ? 5000 : refreshIntervalSec * 1000

  property var status: Model.parseSnapshot("")
  property bool starting: false
  property string actionError: ""
  // Whether the current poll delivered its output; see statusProcess.
  property bool streamed: false

  function localPath(name) {
    return decodeURIComponent(String(Qt.resolvedUrl(name)).replace(/^file:\/\//, ""))
  }

  function setting(name, fallback) {
    var value = settings ? settings[name] : undefined
    return value === undefined || value === null ? fallback : value
  }

  function refresh() {
    if (statusProcess.running) return
    streamed = false
    statusProcess.running = true
    watchdog.restart()
  }

  function syncNow() {
    if (startProcess.running || !Model.canSyncNow(status)) return
    actionError = ""
    starting = true
    startProcess.running = true
  }

  // One hour on the bar: a pause with no end is never one key away. The
  // window offers the other lengths.
  function togglePause() {
    if (controlProcess.running) return
    var command = Model.canResume(status) ? ["syncwatch", "resume"]
                : Model.canPause(status) ? ["syncwatch", "pause", "--minutes", "60"] : null
    if (!command) return
    actionError = ""
    controlProcess.command = command
    controlProcess.running = true
  }

  // The SyncWatch window: focused when open, started otherwise.
  function openHistory() {
    Quickshell.execDetached(["bash", historyPath])
  }

  function openFolder() {
    Quickshell.execDetached(["uwsm-app", "--", "nautilus", drivePath])
  }

  Timer {
    interval: root.pollMs
    repeat: true
    running: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  Process {
    id: statusProcess
    // A fixed argument list: nothing from the shell or the settings reaches it.
    command: ["syncwatch", "status", "--json"]
    // Parse when the stream ends, not on exit: the two signals have no fixed order.
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: { root.streamed = true; root.applyStatus(text) }
    }
    onExited: function(exitCode) {
      if (exitCode !== 0) console.warn("[syncwatch] syncwatch status exited with " + exitCode)
    }
    // A missing binary fails to start: Quickshell then ends the process
    // without ending the stream or emitting exited, so this is the only signal.
    onRunningChanged: { if (!running) { watchdog.stop(); if (!root.streamed) root.applyStatus("") } }
  }

  // A poll that never ends (an old syncwatch opens its window instead of
  // answering) is stopped, and the bar says it does not know. Longer than a
  // cold conflict scan (30 s) plus a slow systemctl (5 s).
  Timer {
    id: watchdog
    interval: 45000
    onTriggered: {
      statusProcess.running = false
      root.applyStatus("")
    }
  }

  function applyStatus(raw) {
    var parsed = Model.parseSnapshot(raw)
    // A missing or old syncwatch shows as "unknown", never as the last good snapshot.
    if (!parsed.ok) console.warn("[syncwatch] syncwatch status printed no snapshot")
    status = parsed
    if (parsed.running) starting = false
  }

  Process {
    id: startProcess
    // Queues one run and returns at once; the run takes minutes.
    command: ["syncwatch", "sync"]
    stderr: StdioCollector { id: startErr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        root.starting = false
        root.actionError = Model.cleanText(startErr.text) || ("syncwatch sync exit " + exitCode)
        console.warn("[syncwatch] syncwatch sync failed: " + root.actionError)
      }
      settle.restart()
    }
  }

  // Pause or resume; the fixed lists in togglePause() are its only commands
  // ["syncwatch", "pause", "--minutes", "60"] and ["syncwatch", "resume"].
  Process {
    id: controlProcess
    stderr: StdioCollector { id: controlErr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        root.actionError = Model.cleanText(controlErr.text) || ("syncwatch exit " + exitCode)
        console.warn("[syncwatch] " + root.actionError)
      }
      root.refresh()
    }
  }

  // systemd takes a moment to flip the unit to activating; look again soon,
  // and give up on "starting" if the run never showed up.
  Timer {
    id: settle
    interval: 1500
    onTriggered: {
      root.refresh()
      giveUp.restart()
    }
  }

  Timer {
    id: giveUp
    interval: 20000
    onTriggered: root.starting = false
  }
}
