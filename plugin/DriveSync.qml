import QtQuick
import QtQuick.Layouts
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "DriveSyncModel.js" as Model

// DriveSync's bar widget for drive-sync. The icon says at a glance how the
// sync is doing; the panel says when, what failed, and runs one now. The
// state comes from the app (`drivesync status --json`), so the bar and the
// window can never disagree.
Panel {
  id: root
  moduleName: "drivesync"
  ipcTarget: "drivesync"
  manageIpc: false

  readonly property var status: sync.status
  readonly property string syncState: Model.barState(status)
  readonly property bool canSync: Model.canSyncNow(status) && !sync.starting
  // The two causes the app names get a sentence; any other error shows its log line.
  readonly property string causeText: status.causeKind === "lostListings" ? strings.t("cause.lostListings")
                                    : status.causeKind === "outOfSync" ? strings.t("cause.outOfSync")
                                    : status.causeDetail

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  property int actionIndex: 0
  property bool cursorActive: false
  // "?" swaps the row of key names, which did not fit, for a list.
  property bool showKeys: false

  readonly property string headline: {
    var s = status
    if (!s.ok) return strings.t("status.unknown")
    switch (s.status) {
      case "ok": return strings.t("status.ok").arg(clock(s.lastSuccess))
      case "stale": return strings.t("status.stale").arg(ago(s.lastSuccess))
      case "failed": return strings.t("status.failed").arg(clock(s.failingSince || s.lastRunStart))
      case "paused":
        if (!s.pausedUntil) return strings.t("status.paused")
        // The mark holds until the next tick clears it.
        return s.pausedUntil <= s.now ? strings.t("status.pausedEnding") : strings.t("status.pausedUntil").arg(clock(s.pausedUntil))
      default: return strings.t("status." + s.status)
    }
  }

  // Epoch seconds as a clock time, with the date when it is not today.
  function clock(sec) {
    if (!sec) return "-"
    var when = new Date(sec * 1000)
    var locale = Qt.locale(Commons.I18n.formatLocale)
    var today = new Date((status.now || sec) * 1000)
    var sameDay = when.toDateString() === today.toDateString()
    return sameDay ? when.toLocaleTimeString(locale, "HH:mm") : when.toLocaleString(locale, "d MMM HH:mm")
  }

  function ago(sec) {
    var parts = Model.ageParts((status.now || sec) - sec)
    if (parts.unit === "now") return strings.t("age.now")
    return strings.tn("age." + parts.unit, parts.value).arg(Commons.I18n.formatNumber(parts.value))
  }

  function duration(seconds) {
    if (seconds < 60) return strings.tn("duration.seconds", seconds).arg(Commons.I18n.formatNumber(seconds))
    var minutes = Math.round(seconds / 60)
    return strings.tn("duration.minutes", minutes).arg(Commons.I18n.formatNumber(minutes))
  }

  function lastRunText() {
    var s = status
    if (s.running && s.currentRunStart) return strings.t("row.runningSince").arg(clock(s.currentRunStart))
    if (!s.lastRunStart) return "-"
    var took = Model.lastRunSeconds(s)
    return took > 0 ? strings.t("row.lastRunValue").arg(clock(s.lastRunStart)).arg(duration(took)) : clock(s.lastRunStart)
  }

  // The app prints 0 when the timer has no next run (off, or a resync paused it).
  function nextRunText() {
    var s = status
    if (s.resync || !s.nextRun) return "-"
    return clock(s.nextRun)
  }

  function runAction(name) {
    if (name === "sync") sync.syncNow()
    else if (name === "pause") sync.togglePause()
    else if (name === "log") { sync.openHistory(); close() }
    else if (name === "folder") { sync.openFolder(); close() }
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: if (opened) {
    cursorActive = false
    showKeys = false
    sync.refresh()
    Qt.callLater(function() { if (panel.loadedContent) panel.loadedContent.forceActiveFocus() })
  }

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  DriveSyncService {
    id: sync
    settings: root.settings
  }

  ShellIpc {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): string { sync.refresh(); return "ok" }
    function syncNow(): string {
      var accepted = root.canSync
      sync.syncNow()
      return accepted ? "started" : "busy"
    }
    function togglePause(): string {
      var answer = Model.canResume(root.status) ? "resumed" : Model.canPause(root.status) ? "paused" : "unavailable"
      sync.togglePause()
      return answer
    }
    // Answers scripts, so it stays English: the state name, not the headline.
    function status(): string { return root.syncState }
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: Model.glyph(root.syncState)
    active: Model.isUrgent(root.syncState)
    dimmed: Model.isDimmed(root.syncState)
    tooltipText: strings.t("tooltip").arg(root.headline)
    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) sync.syncNow()
      else if (buttonCode === Qt.MiddleButton) sync.openFolder()
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(340))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(480))

    content: PanelKeyCatcher {
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      onMoveRequested: function(dx, dy) {
        if (!root.cursorActive) { root.cursorActive = true; return }
        var delta = dx !== 0 ? dx : dy
        if (delta !== 0) root.actionIndex = Model.stepAction(root.actionIndex, delta)
      }
      onActivateRequested: if (root.cursorActive) root.runAction(Model.ACTIONS[root.actionIndex])
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) {
        var key = String(t).toLowerCase()
        if (key === "s") root.runAction("sync")
        else if (key === "p") root.runAction("pause")
        else if (key === "w") root.runAction("log")
        else if (key === "o") root.runAction("folder")
        else if (key === "r") sync.refresh()
        else if (key === "?") root.showKeys = !root.showKeys
      }

      Column {
        id: column
        width: parent.width
        spacing: Style.space(12)

        PanelHero {
          width: parent.width
          title: strings.t("title")
          meta: root.headline
          foreground: root.foreground
          fontFamily: root.fontFamily
          iconOpacity: Model.isDimmed(root.syncState) ? 0.5 : 1.0
          iconComponent: Component {
            Text {
              textFormat: Text.PlainText
              text: Model.glyph(root.syncState)
              color: Model.isUrgent(root.syncState) ? root.urgent : root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.display
            }
          }
        }

        Text {
          textFormat: Text.PlainText
          visible: text !== ""
          width: parent.width
          text: sync.actionError !== "" ? sync.actionError : (root.syncState === "failing" ? root.causeText : "")
          color: root.urgent
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          wrapMode: Text.WordWrap
        }

        Column {
          width: parent.width
          spacing: Style.spacing.labelGap

          InfoPair {
            label: strings.t("row.lastSuccess")
            value: root.status.lastSuccess ? strings.t("row.lastSuccessValue").arg(root.clock(root.status.lastSuccess)).arg(root.ago(root.status.lastSuccess)) : "-"
          }
          InfoPair { label: strings.t("row.lastRun"); value: root.lastRunText() }
          InfoPair { label: strings.t("row.nextRun"); value: root.nextRunText() }
          InfoPair {
            visible: root.status.failingSince > 0
            label: strings.t("row.failingSince")
            value: root.clock(root.status.failingSince)
          }
          InfoPair {
            visible: root.status.conflicts > 0
            label: strings.t("row.conflicts")
            value: Commons.I18n.formatNumber(root.status.conflicts)
          }
        }

        PanelSeparator { foreground: root.foreground }

        RowLayout {
          width: parent.width
          spacing: Style.space(8)

          PanelActionButton {
            iconText: String.fromCodePoint(0xF0450)
            tooltipText: root.canSync ? strings.t("action.sync") : strings.t("action.syncBusy")
            enabled: root.canSync
            hasCursor: root.cursorActive && root.actionIndex === 0
            foreground: root.foreground
            fontFamily: root.fontFamily
            onClicked: root.runAction("sync")
          }
          PanelActionButton {
            readonly property bool resumes: Model.canResume(root.status)
            iconText: String.fromCodePoint(resumes ? 0xF040A : 0xF03E4)  // play, pause
            tooltipText: resumes ? strings.t("action.resume")
                       : Model.canPause(root.status) ? strings.t("action.pause") : strings.t("action.pauseUnavailable")
            enabled: resumes || Model.canPause(root.status)
            hasCursor: root.cursorActive && root.actionIndex === 1
            foreground: root.foreground
            fontFamily: root.fontFamily
            onClicked: root.runAction("pause")
          }
          PanelActionButton {
            iconText: String.fromCodePoint(0xF021A)
            tooltipText: strings.t("action.drivesync")
            hasCursor: root.cursorActive && root.actionIndex === 2
            foreground: root.foreground
            fontFamily: root.fontFamily
            onClicked: root.runAction("log")
          }
          PanelActionButton {
            iconText: String.fromCodePoint(0xF0770)
            tooltipText: strings.t("action.folder")
            hasCursor: root.cursorActive && root.actionIndex === 3
            foreground: root.foreground
            fontFamily: root.fontFamily
            onClicked: root.runAction("folder")
          }

          Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            text: strings.t("keys.toggle")
            color: root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
            elide: Text.ElideRight
          }
        }

        Column {
          visible: root.showKeys
          width: parent.width
          spacing: Style.spacing.labelGap

          // Not L for the window: lowercase h, j, k and l move the cursor
          // (PanelKeyCatcher), so it would only arrive with Shift.
          InfoPair { label: "S"; value: strings.t("keys.sync") }
          InfoPair { label: "P"; value: strings.t("keys.pause") }
          InfoPair { label: "W"; value: strings.t("keys.history") }
          InfoPair { label: "O"; value: strings.t("keys.folder") }
          InfoPair { label: "R"; value: strings.t("keys.refresh") }
          InfoPair { label: strings.t("keys.arrowsLabel"); value: strings.t("keys.arrows") }
          InfoPair { label: "Esc"; value: strings.t("keys.close") }
        }
      }
    }
  }

  component InfoPair: Item {
    property string label: ""
    property string value: ""

    width: parent ? parent.width : 0
    implicitHeight: Math.max(labelText.implicitHeight, valueText.implicitHeight)

    Text {
      id: labelText
      textFormat: Text.PlainText
      anchors.left: parent.left
      text: parent.label
      color: root.foreground
      opacity: 0.6
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
    }

    Text {
      id: valueText
      textFormat: Text.PlainText
      anchors.right: parent.right
      anchors.left: labelText.right
      anchors.leftMargin: Style.space(8)
      horizontalAlignment: Text.AlignRight
      text: parent.value
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
      elide: Text.ElideRight
    }
  }
}
