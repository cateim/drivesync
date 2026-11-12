import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The fix, one moment at a time: what it does before it starts, the dry run
// to confirm, and a step that stopped. The questions about single files have
// their own dialog (FixQuestions.qml).
Popup {
    id: dialog

    // "start" | "confirm" | "stopped"
    readonly property string mode: backend.status === "fixFailed" ? "stopped"
                                 : backend.fixStep === "confirm" ? "confirm"
                                 : "start"
    readonly property bool hasFix: backend.fixStep !== ""
    readonly property int hidden: Math.max(0, backend.fixCopies - backend.fixCopyList.length)
    // The first path shown in the list, moved by the arrows and J K.
    property int top: 0
    onOpened: {
        top = 0
        copies.positionViewAtBeginning()
    }
    // Opened by the fix itself (Main.qml): an Enter meant for something else
    // in the next second must not answer it.
    property bool enterGuard: false
    // Once the base ran, X asks twice: cancelling throws that run away.
    property bool cancelArmed: false
    onClosed: cancelArmed = false
    Timer { id: guardTimer; interval: 1000; onTriggered: dialog.enterGuard = false }

    function openGuarded() {
        enterGuard = true
        guardTimer.restart()
        open()
    }

    function armedCancel() {
        if (backend.fixBaseDone && !cancelArmed) {
            cancelArmed = true
            return false
        }
        return true
    }

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(640, Overlay.overlay ? Overlay.overlay.width - 48 : 640)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    function accept() {
        if (enterGuard)
            return
        if (mode === "start")
            backend.startFix()
        else if (mode === "confirm")
            backend.confirmFix()
        else
            backend.retryFix()
        close()
    }

    function cancelFix() {
        if (!hasFix || !armedCancel())
            return
        backend.cancelFix()
        close()
    }

    function scroll(step) {
        if (!copies.visible)
            return
        top = Math.max(0, Math.min(copies.count - 1, top + step))
        copies.positionViewAtIndex(top, ListView.Beginning)
    }

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 12

        Label {
            text: {
                translator.language
                return dialog.mode === "confirm" ? qsTr("Confirm the base run")
                     : dialog.mode === "stopped" ? qsTr("The fix stopped")
                     : qsTr("Fix the sync")
            }
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }
        Label {
            Layout.fillWidth: true
            // The error comes from rclone's log: never markup.
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: {
                translator.language
                if (dialog.mode === "start")
                    return qsTr("DriveSync rebuilds the base of both sides, the way a resync does, while ~/Drive stays in use. First a dry run lists what the base run would copy, and nothing changes until you confirm it. The base run takes about 40 min; a file that changes in the cloud meanwhile is asked about. Runs wait until the fix ends.")
                if (dialog.mode === "confirm")
                    return backend.fixCopies === 0
                        ? qsTr("The dry run found nothing to copy: the base run only writes new listings.")
                        : qsTr("The dry run says the base run would make %Ln copies, each side getting what it lacks and the newer file winning:", "", backend.fixCopies)
                const why = backend.causeKind !== "fixInterrupted" ? backend.fixError
                    : backend.causeDetail === "the fix stopped halfway" ? qsTr("It stopped halfway: a logout, a shutdown or a kill.")
                    : qsTr("drive-sync-fix.service ended with %1. See: journalctl --user -u drive-sync-fix.service").arg(backend.causeDetail)
                return why + "\n" + qsTr("Retry goes on from the same step. Runs wait until the fix ends or you cancel it.")
            }
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.mode === "confirm" && backend.fixCopies > 0
            // Copies to the notebook are where a file deleted there would come
            // back, so they are counted apart and listed first.
            text: qsTr("To the notebook: %1, listed first. To the cloud: %2.")
                      .arg(Number(backend.fixCopiesToNotebook).toLocaleString(Qt.locale(), "f", 0))
                      .arg(Number(backend.fixCopies - backend.fixCopiesToNotebook).toLocaleString(Qt.locale(), "f", 0))
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: backend.fixCopiesToNotebook > 0 ? "#ffb74d" : "#c4c4cc"
        }
        ListView {
            id: copies
            visible: dialog.mode === "confirm" && count > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 240)
            clip: true
            // At most 500 paths: the backend never hands the whole log over.
            model: dialog.mode === "confirm" ? backend.fixCopyList : []
            delegate: Label {
                required property var modelData
                width: ListView.view.width
                // ↓ to the notebook, ↑ to the cloud.
                text: (modelData.toNotebook ? "↓ " : "↑ ") + modelData.path
                textFormat: Text.PlainText
                font.family: "monospace"
                font.pixelSize: 12
                color: modelData.toNotebook ? "#ffcc80" : "#d4d4dc"
                elide: Text.ElideMiddle
            }
            ScrollBar.vertical: ScrollBar {}
        }
        Label {
            visible: dialog.mode === "confirm" && dialog.hidden > 0
            text: qsTr("And %Ln more.", "", dialog.hidden)
            textFormat: Text.PlainText
            color: "#8a8a94"
            font.pixelSize: 12
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.cancelArmed
            text: qsTr("The base run already ran. Cancelling throws it away, and every run fails until another fix. X again cancels.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#ffb74d"
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Button {
                visible: dialog.hasFix
                text: qsTr("Cancel the fix")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: dialog.cancelFix()
            }
            Label { visible: dialog.hasFix; text: "X"; color: "#6f6f7a"; font.pixelSize: 12 }
            Button {
                text: qsTr("Close")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: dialog.close()
            }
            Label { text: "Esc"; color: "#6f6f7a"; font.pixelSize: 12 }
            Button {
                text: {
                    translator.language
                    return dialog.mode === "confirm" ? qsTr("Run the base")
                         : dialog.mode === "stopped" ? qsTr("Retry")
                         : qsTr("Start the fix")
                }
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
                onClicked: dialog.accept()
            }
            Label { text: "Enter"; color: "#6f6f7a"; font.pixelSize: 12 }
        }
    }
}
