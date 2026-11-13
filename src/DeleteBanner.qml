import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// After --max-delete stopped a run (spec 5b): D shows what the run would
// delete, and lets one run through once the user types how many files
// that is. A fix is never offered here, since it would bring them back.
Rectangle {
    id: banner

    signal reviewRequested()

    readonly property color accent: theme.accent

    visible: deletesHeld
    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    implicitHeight: row.implicitHeight + 24

    // Main.qml passes the same rule the D key follows.
    property bool deletesHeld: false

    RowLayout {
        id: row
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 16 }
        spacing: 12

        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: qsTr("D lists what the run would delete, and lets one run through after you type how many files that is.")
        }
        Button {
            text: qsTr("Review…")
            focusPolicy: Qt.NoFocus
            // Drawn by hand, like the bar banner's button.
            background: Rectangle {
                implicitHeight: 34
                radius: Math.min(4, theme.radius)
                color: parent.down ? Qt.darker(banner.accent, 1.2) : banner.accent
            }
            contentItem: Label {
                text: parent.text
                textFormat: Text.PlainText
                color: "#101010"
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: banner.reviewRequested()
        }
        Label {
            text: "D"
            textFormat: Text.PlainText
            color: "#6f6f7a"
            font.pixelSize: 12
        }
    }
}
