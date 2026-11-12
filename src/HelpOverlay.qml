import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut, shown with "?".
Rectangle {
    id: overlay

    signal closeRequested()

    Material.roundedScale: theme.radius
    color: "#cc000000"

    readonly property var shortcuts: [
        { keys: "Space", what: qsTr("Read the log and ask systemd again") },
        { keys: "Tab   1   2", what: qsTr("Switch between runs and conflict copies") },
        { keys: "↑ ↓   J K", what: qsTr("Move through the list") },
        { keys: "Enter", what: qsTr("Show the picked conflict copy in the file manager") },
        { keys: "S", what: qsTr("Sync now") },
        { keys: "P   U", what: qsTr("Pause, and resume, once DriveSync runs drive-sync") },
        { keys: "T", what: qsTr("Let DriveSync run drive-sync, when offered") },
        { keys: "B", what: qsTr("Put DriveSync on the Omarchy bar, when offered") },
        { keys: "N", what: qsTr("Say no to the bar offer or notice, for good") },
        { keys: "?", what: qsTr("Show or hide these shortcuts") },
        { keys: "Q", what: qsTr("Quit") }
    ]

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.closeRequested()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 560)
        height: list.implicitHeight + 48
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"

        // Clicks on the card don't close it.
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: list
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 24 }
            spacing: 12

            Label {
                text: qsTr("Keyboard shortcuts")
                font.pixelSize: 18
                font.weight: Font.DemiBold
                color: "#f4f4f5"
            }

            Repeater {
                model: overlay.shortcuts
                delegate: RowLayout {
                    id: entry
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 16

                    Rectangle {
                        Layout.preferredWidth: 120
                        implicitHeight: keys.implicitHeight + 8
                        radius: Math.min(4, theme.radius)
                        color: "#2a2a31"
                        Label {
                            id: keys
                            anchors.centerIn: parent
                            text: entry.modelData.keys
                            color: theme.accent
                            font.family: "monospace"
                            font.pixelSize: 13
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: entry.modelData.what
                        wrapMode: Text.Wrap
                        color: "#d4d4dc"
                    }
                }
            }
        }
    }
}
