import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// The finished runs from the log, newest first, beside the one picked.
Item {
    id: page

    property double now
    // The picked run's start, so the pick survives the list being rebuilt
    // when a new run finishes.
    property double pickedStart: 0

    Material.roundedScale: theme.radius

    function move(step) {
        if (list.count > 0)
            list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + step))
    }

    function outcomeTone(outcome) {
        return outcome === "success" ? theme.accent : outcome === "failed" ? "#ff6b6b" : "#ffb74d"
    }

    function outcomeGlyph(outcome) {
        return outcome === "success" ? "✓" : outcome === "failed" ? "✕" : "!"
    }

    function summary(run) {
        translator.language
        if (run.outcome === "failed")
            return run.seconds >= 0 ? qsTr("Failed after %1").arg(Format.duration(run.seconds)) : qsTr("Failed")
        if (run.outcome === "interrupted")
            return qsTr("Cut off")
        const changes = run.changeCount > 0 ? qsTr("%Ln changes", "", run.changeCount) : qsTr("No changes")
        return qsTr("%1 · %2").arg(Format.duration(run.seconds)).arg(changes)
    }

    Connections {
        target: backend
        function onRunsChanged() {
            Qt.callLater(() => {
                for (let i = 0; i < backend.runs.length; ++i)
                    if (backend.runs[i].start.getTime() === page.pickedStart) {
                        list.currentIndex = i
                        return
                    }
                list.currentIndex = backend.runs.length ? 0 : -1
            })
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 14

        Rectangle {
            Layout.preferredWidth: Math.max(280, page.width * 0.34)
            Layout.fillHeight: true
            color: "#16161a"
            radius: theme.radius

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                model: backend.runs
                currentIndex: 0
                highlightMoveDuration: 0
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                onCurrentIndexChanged: {
                    if (currentIndex >= 0 && currentIndex < backend.runs.length)
                        page.pickedStart = backend.runs[currentIndex].start.getTime()
                }

                delegate: ItemDelegate {
                    id: row
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 56
                    highlighted: ListView.isCurrentItem
                    onClicked: list.currentIndex = index

                    contentItem: RowLayout {
                        spacing: 12

                        Label {
                            Layout.preferredWidth: 18
                            horizontalAlignment: Text.AlignHCenter
                            text: page.outcomeGlyph(row.modelData.outcome)
                            color: page.outcomeTone(row.modelData.outcome)
                            font.pixelSize: 16
                            font.weight: Font.Bold
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                text: { translator.language; return Format.when(row.modelData.start, page.now, clockFormat.timeFormat) }
                                color: "#ececf1"
                                font.pixelSize: 15
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: page.summary(row.modelData)
                                color: "#9a9aa6"
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            visible: row.modelData.resync
                            text: qsTr("resync")
                            color: "#9a9aa6"
                            font.pixelSize: 11
                            font.capitalization: Font.AllUppercase
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 40
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("No finished runs in the log yet.")
                color: "#8a8a96"
            }
        }

        RunDetail {
            Layout.fillWidth: true
            Layout.fillHeight: true
            now: page.now
            run: list.currentIndex >= 0 && list.currentIndex < backend.runs.length ? backend.runs[list.currentIndex] : null
        }
    }
}
