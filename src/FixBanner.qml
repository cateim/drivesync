import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Offers the fix when a run failed in a way only a new base repairs: lost
// listings, listings out of sync, or filters edited since the last base.
// Nothing starts until the user confirms in the dialog that F opens.
Rectangle {
    id: banner

    signal fixRequested()

    readonly property color accent: theme.accent
    readonly property bool needsBase: backend.status === "failed"
        && ["lostListings", "outOfSync", "filtersChanged"].indexOf(backend.causeKind) >= 0

    visible: needsBase && backend.managed && backend.fixStep === ""
    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    implicitHeight: row.implicitHeight + 24

    RowLayout {
        id: row
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 16 }
        spacing: 12

        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: qsTr("F rebuilds the base of both sides: about 40 min, and the folder stays in use.")
        }
        Button {
            text: qsTr("Fix…")
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
            onClicked: banner.fixRequested()
        }
        Label {
            text: "F"
            textFormat: Text.PlainText
            color: "#6f6f7a"
            font.pixelSize: 12
        }
    }
}
