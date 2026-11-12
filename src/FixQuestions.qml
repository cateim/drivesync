import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// The files that changed in the cloud while the base run listed both sides
// (spec section 5, step 5). Each one gets an answer, and nothing goes on
// until every one has one: Enter sends them all at once.
Popup {
    id: dialog

    property double now: Date.now()
    // One answer per question, by position: an object keyed by file name
    // would trip on names like "__proto__" or "constructor".
    property var answers: []
    property int current: 0
    readonly property var questions: backend.fixQuestions
    readonly property int answered: {
        let count = 0
        for (let i = 0; i < questions.length; ++i)
            if (answers[i])
                ++count
        return count
    }
    readonly property bool complete: questions.length > 0 && answered === questions.length
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
    width: Math.min(720, Overlay.overlay ? Overlay.overlay.width - 48 : 720)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    // Another set of questions (a retried base run) starts over.
    onQuestionsChanged: {
        answers = []
        current = 0
    }
    onOpened: now = Date.now()

    function move(step) {
        if (questions.length === 0)
            return
        current = Math.max(0, Math.min(questions.length - 1, current + step))
        list.positionViewAtIndex(current, ListView.Contain)
    }

    function choose(side) {
        if (questions.length === 0)
            return
        const next = answers.slice()
        next[current] = side
        answers = next
        move(1)
    }

    function send() {
        if (!complete || enterGuard)
            return
        backend.answerFix(answers.map(answer => answer || ""))
        close()
    }

    function cancelFix() {
        if (!armedCancel())
            return
        backend.cancelFix()
        close()
    }

    function side(time, size) {
        return qsTr("%1, %2").arg(Format.when(time, now, clockFormat.timeFormat)).arg(Format.size(size))
    }

    function answerText(question, index) {
        translator.language
        const answer = answers[index]
        if (answer === "cloud")
            return qsTr("Keep the cloud's")
        if (answer === "notebook")
            return question.kind === "onlyCloud" ? qsTr("Send to the Drive trash") : qsTr("Keep the notebook's")
        return qsTr("No answer yet")
    }

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 12

        Label {
            text: qsTr("Files that changed during the base run")
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: qsTr("These changed in the cloud while the base run listed both sides. For each one, keep the cloud's version (C) or the notebook's (L); a file only in the cloud goes to the Drive trash with L. Files newer on the notebook were kept without asking.")
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 300)
            clip: true
            spacing: 2
            model: dialog.questions
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                implicitHeight: lines.implicitHeight + 12
                radius: Math.min(4, theme.radius)
                color: index === dialog.current ? "#26262d" : "transparent"

                ColumnLayout {
                    id: lines
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 8 }
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            Layout.fillWidth: true
                            // File names are whatever the user named them: never markup.
                            text: row.modelData.path
                            textFormat: Text.PlainText
                            font.family: "monospace"
                            font.pixelSize: 13
                            color: "#f4f4f5"
                            elide: Text.ElideMiddle
                        }
                        Label {
                            text: dialog.answerText(row.modelData, row.index)
                            textFormat: Text.PlainText
                            font.pixelSize: 13
                            color: dialog.answers[row.index] ? theme.accent : "#8a8a94"
                        }
                    }
                    Label {
                        text: qsTr("Cloud: %1").arg(dialog.side(row.modelData.cloudTime, row.modelData.cloudSize))
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: "#a4a4ae"
                    }
                    Label {
                        text: row.modelData.kind === "onlyCloud" ? qsTr("Notebook: not there")
                            : qsTr("Notebook: %1").arg(dialog.side(row.modelData.notebookTime, row.modelData.notebookSize))
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: "#a4a4ae"
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: dialog.current = row.index
                }
            }
            ScrollBar.vertical: ScrollBar {}
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
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Answered: %1 of %2").arg(dialog.answered).arg(dialog.questions.length)
                textFormat: Text.PlainText
                color: "#8a8a94"
                font.pixelSize: 12
            }
            Button {
                text: qsTr("Cancel the fix")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: dialog.cancelFix()
            }
            Label { text: "X"; color: "#6f6f7a"; font.pixelSize: 12 }
            Button {
                text: qsTr("Close")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: dialog.close()
            }
            Label { text: "Esc"; color: "#6f6f7a"; font.pixelSize: 12 }
            Button {
                text: qsTr("Send the answers")
                enabled: dialog.complete
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    implicitHeight: 34
                    radius: Math.min(4, theme.radius)
                    color: !parent.enabled ? "#3a3a42" : parent.down ? Qt.darker(theme.accent, 1.2) : theme.accent
                }
                contentItem: Label {
                    text: parent.text
                    textFormat: Text.PlainText
                    color: parent.enabled ? "#101010" : "#8a8a94"
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: dialog.send()
            }
            Label { text: "Enter"; color: "#6f6f7a"; font.pixelSize: 12 }
        }
    }
}
