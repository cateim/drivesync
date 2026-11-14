import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What a run stopped by --max-delete would delete, on which side, and the
// typed count that lets one run through (spec 5b). The run after it has the
// guard again; DriveSync checks with a dry run that nothing more goes.
Popup {
    id: dialog

    readonly property int count: backend.blockedCount
    readonly property int hidden: Math.max(0, count - backend.blockedList.length)
    // The number, typed by hand: it cannot be typed without reading it.
    readonly property bool matches: count > 0 && field.text === String(count)
    // The first path shown in the list, moved by the arrows.
    property int top: 0

    // The field takes the keys: Esc, Enter and the arrows reach the window's
    // shortcuts, every other window shortcut is off while a dialog is open.
    focus: true
    modal: true
    closePolicy: Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(640, Overlay.overlay ? Overlay.overlay.width - 48 : 640)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    onOpened: {
        top = 0
        paths.positionViewAtBeginning()
        field.text = ""
        field.forceActiveFocus()
    }
    onClosed: field.text = ""

    function accept() {
        if (!matches)
            return
        backend.allowDeletes(parseInt(field.text, 10))
        close()
    }

    function scroll(step) {
        if (!paths.visible)
            return
        top = Math.max(0, Math.min(paths.count - 1, top + step))
        paths.positionViewAtIndex(top, ListView.Beginning)
    }

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 12

        Label {
            text: qsTr("Review the deletions")
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: qsTr("The last run stopped before deleting anything: it would delete %Ln files, more than the max-delete guard allows.", "", dialog.count)
        }
        // A file gone from the cloud is deleted on the notebook for good, so
        // that side is counted apart, listed first and marked.
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: backend.blockedCloudCount > 0 ? "#ffb74d" : "#c4c4cc"
            text: "↓ " + qsTr("Gone from the cloud: %1. Deleted on the notebook, for good.")
                             .arg(Number(backend.blockedCloudCount).toLocaleString(Qt.locale(), "f", 0))
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: "↑ " + qsTr("Gone from the notebook: %1. Sent to the Drive trash.")
                             .arg(Number(backend.blockedNotebookCount).toLocaleString(Qt.locale(), "f", 0))
        }
        ListView {
            id: paths
            visible: count > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 220)
            clip: true
            // At most 500 paths: the backend never hands the whole log over.
            model: backend.blockedList
            delegate: Label {
                required property var modelData
                width: ListView.view.width
                text: (modelData.deletedInCloud ? "↓ " : "↑ ") + modelData.path
                textFormat: Text.PlainText
                font.family: "monospace"
                font.pixelSize: 12
                color: modelData.deletedInCloud ? "#ffcc80" : "#d4d4dc"
                elide: Text.ElideMiddle
            }
            ScrollBar.vertical: ScrollBar {}
        }
        Label {
            visible: dialog.hidden > 0
            // The window holds 500; the command prints every one.
            text: qsTr("And %Ln more.", "", dialog.hidden) + " " + qsTr("All of them: drive-sync deletes --list")
            textFormat: Text.PlainText
            color: "#8a8a94"
            font.pixelSize: 12
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            // The number as typed, with no thousands separator.
            text: qsTr("Type %1 to let one run delete them. The run after it has the guard again.").arg(String(dialog.count))
        }
        TextField {
            id: field
            Layout.preferredWidth: 160
            validator: IntValidator { bottom: 0 }
            inputMethodHints: Qt.ImhDigitsOnly
            maximumLength: 9
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Button {
                text: qsTr("Close")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: dialog.close()
            }
            Label { text: "Esc"; color: "#6f6f7a"; font.pixelSize: 12 }
            Button {
                text: qsTr("Let one run delete them")
                enabled: dialog.matches
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    implicitHeight: 34
                    radius: Math.min(4, theme.radius)
                    color: !parent.enabled ? "#2e2e36" : parent.down ? Qt.darker(theme.accent, 1.2) : theme.accent
                }
                contentItem: Label {
                    text: parent.text
                    textFormat: Text.PlainText
                    color: parent.enabled ? "#101010" : "#6f6f7a"
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: dialog.accept()
            }
            Label { text: "Enter"; color: "#6f6f7a"; font.pixelSize: 12 }
        }
    }
}
