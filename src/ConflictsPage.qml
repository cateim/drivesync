import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// The copies bisync renamed when a file changed on both sides.
Rectangle {
    id: page

    property double now

    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius

    function move(step) {
        if (list.count > 0)
            list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + step))
    }

    function openCurrent() {
        if (list.currentIndex >= 0 && list.currentIndex < backend.conflicts.length)
            backend.showInFileManager(backend.conflicts[list.currentIndex].path)
    }

    function emptyText() {
        translator.language
        const drive = Format.home(backend.drivePath)
        switch (backend.conflictError) {
        case "noDrive": return qsTr("%1 isn't there, so there is nothing to look through.").arg(drive)
        case "noFind": return qsTr("Can't look for conflict copies: find isn't installed.")
        case "scanFailed": return qsTr("Looking through %1 for conflict copies failed.").arg(drive)
        }
        return qsTr("No conflict copies in %1.").arg(drive)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("When a file changes on both sides, bisync keeps the newer one under the original name and renames the other with .conflict and a number. Compare the two, keep what you want, then delete the copy.")
            wrapMode: Text.Wrap
            color: "#9a9aa6"
            font.pixelSize: 13
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.conflicts
            currentIndex: 0
            highlightMoveDuration: 0
            keyNavigationEnabled: false
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 56
                highlighted: ListView.isCurrentItem
                onClicked: list.currentIndex = index
                onDoubleClicked: {
                    list.currentIndex = index
                    page.openCurrent()
                }

                contentItem: RowLayout {
                    spacing: 14

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            color: "#ececf1"
                            font.pixelSize: 15
                            elide: Text.ElideMiddle
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.folder !== "" ? row.modelData.folder : Format.home(backend.drivePath)
                            color: "#9a9aa6"
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }

                    Label {
                        text: { translator.language; return Format.ago(row.modelData.modified, page.now) + "  " + Format.size(row.modelData.size) }
                        color: "#9a9aa6"
                        font.pixelSize: 12
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 40
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: page.emptyText()
                color: backend.conflictError === "" ? "#8a8a96" : "#ff8a80"
            }
        }

        Label {
            Layout.fillWidth: true
            visible: list.count > 0
            text: qsTr("Enter shows the picked copy in the file manager.")
            color: "#8a8a96"
            font.pixelSize: 12
        }
    }
}
