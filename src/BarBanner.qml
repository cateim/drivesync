import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Offers to put DriveSync's widget on the Omarchy bar, or says why it could
// not. Nothing changes on the bar until the user presses the button or B.
Rectangle {
    id: banner

    // Not "state": every Item already has one.
    readonly property string setupState: pluginSetup.state
    readonly property bool offering: setupState === "offer" || setupState === "replaceOffer"
    readonly property color accent: theme.accent

    visible: offering || setupState === "blocked" || setupState === "failed"
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
            // The detail can be a path or what omarchy printed: never markup.
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: banner.setupState === "offer" ? qsTr("Show DriveSync on the Omarchy bar, with the state at a glance.")
                : banner.setupState === "replaceOffer" ? qsTr("DriveSync can take the place of the old Drive widget on the bar.")
                : banner.setupState === "blocked" ? qsTr("%1 already exists and is not DriveSync's, so the bar was left alone.").arg(pluginSetup.detail)
                : qsTr("Could not put DriveSync on the bar: %1").arg(pluginSetup.detail)
        }
        Button {
            visible: banner.offering
            text: banner.setupState === "replaceOffer" ? qsTr("Replace the old Drive widget") : qsTr("Show in the bar")
            focusPolicy: Qt.NoFocus
            // Drawn by hand: the Material button background is an elevation
            // effect that does not show up everywhere, and this one action
            // must read as a button.
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
            onClicked: pluginSetup.accept()
        }
        Label {
            visible: banner.offering
            text: "B"
            textFormat: Text.PlainText
            color: "#6f6f7a"
            font.pixelSize: 12
        }
        Button {
            visible: banner.offering
            text: qsTr("No, thanks")
            flat: true
            focusPolicy: Qt.NoFocus
            onClicked: pluginSetup.decline()
        }
        Label {
            visible: banner.offering
            text: "N"
            textFormat: Text.PlainText
            color: "#6f6f7a"
            font.pixelSize: 12
        }
    }
}
