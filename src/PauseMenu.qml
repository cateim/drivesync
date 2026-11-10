import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// How long to pause: the user always picks, so a pause with no end is never
// one key away.
Popup {
    id: menu

    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    function choose(index) {
        if (index === 1)
            backend.pauseFor(60)
        else if (index === 2)
            backend.pauseUntilTomorrow()
        else if (index === 3)
            backend.pauseUntilResumed()
        close()
    }

    readonly property var choices: [
        { key: "1", what: qsTr("For one hour") },
        { key: "2", what: qsTr("Until 8:00 in the morning") },
        { key: "3", what: qsTr("Until I resume") }
    ]

    ColumnLayout {
        spacing: 12

        Label {
            text: qsTr("Pause syncing")
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }
        Label {
            Layout.preferredWidth: 380
            text: qsTr("The timer keeps ticking and each run is skipped until the pause ends, even after a reboot.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
        }
        Repeater {
            model: menu.choices
            delegate: RowLayout {
                id: choice
                required property var modelData
                required property int index
                spacing: 12
                Rectangle {
                    implicitWidth: 28
                    implicitHeight: 24
                    radius: Math.min(4, theme.radius)
                    color: "#2a2a31"
                    Label {
                        anchors.centerIn: parent
                        text: choice.modelData.key
                        color: theme.accent
                        font.family: "monospace"
                    }
                }
                Button {
                    text: choice.modelData.what
                    flat: true
                    focusPolicy: Qt.NoFocus
                    onClicked: menu.choose(choice.index + 1)
                }
            }
        }
        Label {
            text: qsTr("Esc cancels")
            color: "#6f6f7a"
            font.pixelSize: 12
        }
    }
}
