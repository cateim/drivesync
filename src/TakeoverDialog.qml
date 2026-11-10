import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// What a takeover changes, before it changes anything.
Popup {
    id: dialog

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(568, Overlay.overlay ? Overlay.overlay.width - 48 : 568)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    function confirm() {
        takeoverSetup.accept()
        close()
    }

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 12

        Label {
            text: qsTr("Take over drive-sync")
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("SyncWatch's own units will run the same rclone command, with the same remote, folder, filters and listings, so no resync is needed. These files move to ~/.local/share/syncwatch/taken-over, nothing is deleted:")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
        }
        Label {
            Layout.fillWidth: true
            text: takeoverSetup.found.map(path => "  " + Format.home(path)).join("\n")
            textFormat: Text.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            color: "#d4d4dc"
            elide: Text.ElideMiddle
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("To go back: syncwatch takeover --undo")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Button {
                text: qsTr("Cancel")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: dialog.close()
            }
            Label { text: "Esc"; color: "#6f6f7a"; font.pixelSize: 12 }
            Button {
                text: qsTr("Take over")
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    implicitHeight: 34
                    radius: Math.min(4, theme.radius)
                    color: parent.down ? Qt.darker(theme.accent, 1.2) : theme.accent
                }
                contentItem: Label {
                    text: parent.text
                    textFormat: Text.PlainText
                    color: "#101010"
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: dialog.confirm()
            }
            Label { text: "Enter"; color: "#6f6f7a"; font.pixelSize: 12 }
        }
    }
}
