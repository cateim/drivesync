import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// One run: how it ended, how many files changed on each side, and which.
Rectangle {
    id: detail

    property var run: null
    property double now

    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    clip: true

    function outcomeLine() {
        translator.language
        if (!run)
            return ""
        let line
        if (run.outcome === "success")
            line = qsTr("Synced in %1.").arg(Format.duration(run.seconds))
        else if (run.outcome === "failed")
            line = run.seconds >= 0 ? qsTr("Failed after %1.").arg(Format.duration(run.seconds)) : qsTr("Failed.")
        else
            line = qsTr("Cut off: the log stops before the run's end line.")
        if (run.resync)
            line += " " + qsTr("This was a resync, which rebuilds the base of both sides.")
        return line
    }

    function causeText() {
        translator.language
        switch (run ? run.causeKind : "") {
        // Same sentences as the headline (StatusPanel.qml), managed or not.
        case "lostListings": return backend.managed
            ? qsTr("bisync lost its listings, the base of both sides. Press F to fix it.")
            : qsTr("bisync lost its listings, the base of both sides. To recover: drive-sync.sh resync")
        case "outOfSync": return backend.managed
            ? qsTr("The listings of the two sides don't match, so bisync dropped its base. Press F to fix it.")
            : qsTr("The listings of the two sides don't match, so bisync dropped its base. To recover: drive-sync.sh resync")
        case "filtersChanged": return qsTr("The filters changed since the last base, so every run waits. Press F to rebuild the base with them.")
        case "maxDelete": return qsTr("%1\nA fix would bring the deleted files back, so F is off. Restore them, or let one run through by hand with --force.").arg(run.causeDetail)
        case "error": return run.causeDetail
        case "none": return qsTr("The log has no detail about it.")
        }
        return ""
    }

    function countsLine(counts) {
        translator.language
        return qsTr("%1 new, %2 changed, %3 deleted").arg(counts.added).arg(counts.modified).arg(counts.deleted)
    }

    function sideTitle(side) {
        translator.language
        return side === "cloud" ? qsTr("Cloud → notebook")
             : side === "notebook" ? qsTr("Notebook → cloud")
             : qsTr("Changed on both sides")
    }

    function kindText(kind) {
        translator.language
        return kind === "new" ? qsTr("new") : kind === "modified" ? qsTr("changed")
             : kind === "deleted" ? qsTr("deleted") : qsTr("conflict")
    }

    function kindTone(kind) {
        return kind === "new" ? theme.accent : kind === "deleted" ? "#ff8a80" : kind === "conflict" ? "#ffb74d" : "#90caf9"
    }

    Label {
        anchors.centerIn: parent
        visible: !detail.run
        text: qsTr("Pick a run in the list.")
        color: "#8a8a96"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12
        visible: !!detail.run

        Label {
            Layout.fillWidth: true
            text: detail.run ? (translator.language, qsTr("Run at %1").arg(Format.when(detail.run.start, detail.now, clockFormat.timeFormat))) : ""
            font.pixelSize: 19
            font.weight: Font.DemiBold
            color: "#f4f4f5"
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            text: detail.outcomeLine()
            wrapMode: Text.Wrap
            color: "#c4c4cc"
        }

        Label {
            Layout.fillWidth: true
            visible: text !== ""
            text: detail.run && detail.run.outcome === "failed" ? detail.causeText() : ""
            wrapMode: Text.Wrap
            color: "#ff8a80"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            // A run that failed before comparing the sides has no counts to show.
            visible: !!detail.run && !detail.run.resync && (detail.run.outcome === "success" || detail.run.changeCount > 0)

            Repeater {
                model: ["cloud", "notebook"]
                delegate: Rectangle {
                    id: sideBox
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1  // Both boxes get the same share.
                    implicitHeight: box.implicitHeight + 20
                    radius: theme.radius
                    color: "#1d1d22"

                    ColumnLayout {
                        id: box
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 12 }
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: detail.sideTitle(sideBox.modelData)
                            color: "#ececf1"
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: detail.run ? detail.countsLine(detail.run[sideBox.modelData]) : ""
                            color: "#9a9aa6"
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }

        ListView {
            id: changes
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: detail.run ? detail.run.changes : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Column {
                id: change
                required property var modelData
                required property int index
                width: ListView.view.width

                Label {
                    visible: change.index === 0 || detail.run.changes[change.index - 1].side !== change.modelData.side
                    text: detail.sideTitle(change.modelData.side)
                    color: "#9a9aa6"
                    font.pixelSize: 12
                    font.capitalization: Font.AllUppercase
                    topPadding: change.index === 0 ? 0 : 12
                    bottomPadding: 4
                }

                RowLayout {
                    width: parent.width
                    height: 26
                    spacing: 10

                    Label {
                        Layout.preferredWidth: 72
                        text: detail.kindText(change.modelData.kind)
                        color: detail.kindTone(change.modelData.kind)
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: change.modelData.path
                        color: "#d4d4dc"
                        font.pixelSize: 13
                        elide: Text.ElideMiddle
                    }
                }
            }

            footer: Label {
                width: ListView.view.width
                visible: !!detail.run && detail.run.changesOmitted > 0
                height: visible ? implicitHeight + 12 : 0
                topPadding: 8
                text: detail.run ? qsTr("And %Ln more.", "", detail.run.changesOmitted) : ""
                color: "#9a9aa6"
            }

            Label {
                anchors.centerIn: parent
                visible: changes.count === 0 && !!detail.run && detail.run.outcome === "success"
                text: qsTr("Nothing changed on either side.")
                color: "#8a8a96"
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(130, errorsText.implicitHeight + 8)
            Layout.minimumHeight: Math.min(60, errorsText.implicitHeight + 8)
            visible: !!detail.run && detail.run.errors.length > 0

            TextArea {
                id: errorsText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                font.family: "monospace"
                font.pixelSize: 12
                color: "#ffab91"
                text: detail.run ? detail.run.errors.join("\n") : ""
                background: Rectangle { color: "#201a1a"; radius: Math.min(4, theme.radius) }
            }
        }
    }
}
