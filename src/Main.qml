import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1000
    height: 700
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "SyncWatch"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property int page: 0  // 0: runs, 1: conflict copies
    // Relative times ("8 min ago") are computed against this clock.
    property double now: Date.now()

    Timer {
        interval: 15000
        running: true
        repeat: true
        onTriggered: win.now = Date.now()
    }
    Connections {
        target: backend
        function onChanged() { win.now = Date.now() }
    }

    function move(step) {
        if (win.page === 0)
            runsPage.move(step)
        else
            conflictsPage.move(step)
    }

    // "?" arrives with Shift on most layouts (US, ABNT2), and not every
    // platform plugin matches it against the plain "?".
    Shortcut { sequences: ["?", "Shift+?"]; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: backend.refresh() }
    Shortcut {
        sequence: "B"
        context: Qt.ApplicationShortcut
        enabled: pluginSetup.state === "offer" || pluginSetup.state === "replaceOffer"
        onActivated: pluginSetup.accept()
    }
    Shortcut {
        sequence: "N"
        context: Qt.ApplicationShortcut
        enabled: pluginSetup.state === "offer" || pluginSetup.state === "replaceOffer"
        onActivated: pluginSetup.decline()
    }
    Shortcut { sequences: ["Tab", "Backtab"]; context: Qt.ApplicationShortcut; onActivated: win.page = 1 - win.page }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; onActivated: win.page = 0 }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; onActivated: win.page = 1 }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; onActivated: win.move(1) }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; onActivated: win.move(-1) }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: win.page === 1
        onActivated: conflictsPage.openCurrent()
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            StatusPanel {
                Layout.fillWidth: true
                now: win.now
            }

            BarBanner {
                Layout.fillWidth: true
            }

            TabBar {
                id: tabs
                Layout.fillWidth: true
                currentIndex: win.page
                onCurrentIndexChanged: win.page = currentIndex
                Material.background: "transparent"

                TabButton {
                    text: qsTr("Runs")
                    focusPolicy: Qt.NoFocus
                }
                TabButton {
                    text: backend.conflicts.length > 0 ? qsTr("Conflict copies (%1)").arg(backend.conflicts.length)
                                                       : qsTr("Conflict copies")
                    focusPolicy: Qt.NoFocus
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: win.page

                RunsPage {
                    id: runsPage
                    now: win.now
                }
                ConflictsPage {
                    id: conflictsPage
                    now: win.now
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: backend.message
                    color: theme.accent
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("Space refreshes   ? shortcuts")
                    color: "#6f6f7a"
                    font.pixelSize: 12
                }
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onCloseRequested: win.helpVisible = false
        }
    }
}
