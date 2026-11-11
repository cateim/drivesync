import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sync now, pause and resume, each with its key. Pause and resume need
// DriveSync to run drive-sync, so they show only after a takeover.
RowLayout {
    id: row

    signal pauseRequested()

    spacing: 8

    component KeyButton: RowLayout {
        id: control
        property alias text: button.text
        property string key
        signal activated()
        spacing: 4
        Button {
            id: button
            flat: true
            focusPolicy: Qt.NoFocus
            Material.foreground: theme.accent
            onClicked: control.activated()
        }
        Label {
            text: control.key
            textFormat: Text.PlainText
            color: "#6f6f7a"
            font.pixelSize: 12
        }
    }

    KeyButton {
        text: qsTr("Sync now")
        key: "S"
        enabled: backend.canSyncNow
        opacity: enabled ? 1 : 0.4
        onActivated: backend.syncNow()
    }
    KeyButton {
        visible: backend.managed && !backend.paused
        text: qsTr("Pause…")
        key: "P"
        onActivated: row.pauseRequested()
    }
    KeyButton {
        visible: backend.paused
        text: qsTr("Resume")
        key: "U"
        onActivated: backend.resume()
    }
    Item { Layout.fillWidth: true }
}
