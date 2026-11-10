import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Offers to run drive-sync from SyncWatch, or says why it cannot yet.
// Nothing changes until the user confirms in the dialog that T opens.
Rectangle {
    id: banner

    signal confirmRequested()

    // Not "state": every Item already has one.
    readonly property string setupState: takeoverSetup.state
    readonly property color accent: theme.accent

    visible: setupState === "offer" || setupState === "blocked" || setupState === "failed" || setupState === "working"
    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    implicitHeight: row.implicitHeight + 24

    function blockerText(code) {
        translator.language
        const name = code.indexOf(":") > 0 ? code.slice(code.indexOf(":") + 1) : ""
        if (code === "running") return qsTr("a run is going, and the offer comes back when it ends")
        if (code === "lastRunNotSuccess") return qsTr("the last run did not succeed")
        if (code === "noListings") return qsTr("bisync has no listings to keep")
        if (code === "filtersChanged") return qsTr("the filters changed since the last resync")
        if (code === "remoteMissing") return qsTr("rclone does not know the remote")
        if (code === "unreadableUnit") return qsTr("drive-sync.service runs something SyncWatch cannot read")
        if (code === "targetExists") return qsTr("~/.config/syncwatch already has filters")
        if (code.startsWith("unknownDirective:")) return qsTr("drive-sync has %1, which SyncWatch cannot keep").arg(name)
        if (code.startsWith("differentValue:")) return qsTr("%1 differs from SyncWatch's unit").arg(name)
        return code
    }

    RowLayout {
        id: row
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 16 }
        spacing: 12

        Label {
            Layout.fillWidth: true
            // Blockers and errors carry paths and what systemd printed: never markup.
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: {
                translator.language
                switch (banner.setupState) {
                case "offer": return qsTr("SyncWatch can run drive-sync itself, with notifications, pause and resume. The same rclone command keeps running, with no resync.")
                case "working": return qsTr("Taking over drive-sync…")
                case "blocked": return qsTr("SyncWatch cannot take over drive-sync yet: %1.")
                                    .arg(takeoverSetup.blockers.map(code => banner.blockerText(code)).join("; "))
                }
                return qsTr("The takeover did not happen: %1").arg(takeoverSetup.detail)
            }
        }
        Button {
            visible: banner.setupState === "offer"
            text: qsTr("Take over…")
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
            onClicked: banner.confirmRequested()
        }
        Label {
            visible: banner.setupState === "offer"
            text: "T"
            textFormat: Text.PlainText
            color: "#6f6f7a"
            font.pixelSize: 12
        }
    }
}
