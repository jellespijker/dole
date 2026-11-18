import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The pattern being edited: a header for each track, then its sixteen steps.
Item {
    id: grid

    property int cursorTrack: 0
    property int cursorStep: 0
    property bool active: true

    signal cellPicked(int track, int step)
    signal trackPicked(int track)

    readonly property int headerWidth: 210
    readonly property int rulerHeight: 18
    readonly property int rowSpacing: 6
    readonly property real rowHeight: Math.max(30, Math.min(76, (height - rulerHeight - 7 * rowSpacing) / 8))

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    function trackName(id) {
        switch (id) {
        case "kick": return qsTr("Kick")
        case "snare": return qsTr("Snare")
        case "hat": return qsTr("Hat")
        case "openhat": return qsTr("Open hat")
        case "clap": return qsTr("Clap")
        case "rim": return qsTr("Rim")
        case "cowbell": return qsTr("Cowbell")
        case "bass": return qsTr("Bass")
        }
        return id
    }

    function soundText(track) {
        switch (track.state) {
        case "builtin": return qsTr("Built-in")
        case "loading": return qsTr("Loading %1…").arg(track.sample)
        }
        return track.sample
    }

    // Steps are time, which reads left to right in every language, so the
    // step area places its cells by x and doesn't mirror. Its parent is
    // anchored, mirrored, beside the track headers; it fills that parent.
    component StepArea: Item {
        anchors.fill: parent
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        readonly property real gap: 4
        readonly property real groupGap: 10
        readonly property real cellWidth: (width - 12 * gap - 3 * groupGap) / 16
        function cellX(step) { return step * (cellWidth + gap) + Math.floor(step / 4) * (groupGap - gap) }
    }

    Column {
        anchors.fill: parent
        spacing: grid.rowSpacing

        // Beat numbers over the steps.
        Item {
            width: grid.width
            height: grid.rulerHeight

            Item {
                anchors.fill: parent
                anchors.leftMargin: grid.headerWidth + 12

                StepArea {
                    id: ruler

                    Repeater {
                        model: 16
                        delegate: Label {
                            required property int index
                            x: ruler.cellX(index)
                            width: ruler.cellWidth
                            horizontalAlignment: Text.AlignHCenter
                            text: index % 4 === 0 ? (index / 4 + 1).toLocaleString(Qt.locale(translator.language), 'f', 0) : "·"
                            color: backend.playing && backend.playStep === index && backend.playPattern === backend.editPattern
                                   ? theme.accent : theme.colors.muted
                            font.pixelSize: 11
                            font.bold: index % 4 === 0
                        }
                    }
                }
            }
        }

        Repeater {
            model: backend.tracks

            delegate: Item {
                id: row
                required property int index
                required property var modelData
                readonly property int track: index
                readonly property bool current: grid.active && grid.cursorTrack === index

                width: grid.width
                height: grid.rowHeight
                opacity: modelData.audible ? 1 : 0.45

                Rectangle {
                    id: header
                    objectName: "trackHeader" + row.index
                    anchors.left: parent.left
                    width: grid.headerWidth
                    height: parent.height
                    radius: Math.min(6, theme.radius)
                    color: row.current ? grid.alpha(theme.colors.selection, 0.7) : grid.alpha(theme.colors.selection, 0.25)

                    MouseArea {
                        anchors.fill: parent
                        onClicked: grid.trackPicked(row.index)
                        onWheel: (wheel) => backend.changeVolume(row.index, wheel.angleDelta.y > 0 ? 0.05 : -0.05)
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: grid.trackName(row.modelData.id)
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Repeater {
                                model: [
                                    { key: "M", on: row.modelData.mute, color: theme.colors.red },
                                    { key: "S", on: row.modelData.solo, color: theme.colors.yellow }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    implicitWidth: 20
                                    implicitHeight: 18
                                    radius: Math.min(4, theme.radius)
                                    color: modelData.on ? modelData.color : "transparent"
                                    border.color: modelData.on ? modelData.color : theme.colors.muted
                                    Label {
                                        anchors.centerIn: parent
                                        text: parent.modelData.key
                                        font.pixelSize: 11
                                        font.bold: true
                                        color: parent.modelData.on ? theme.background : theme.colors.muted
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: parent.index === 0 ? backend.toggleMute(row.index) : backend.toggleSolo(row.index)
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: grid.rowHeight >= 40

                            Label {
                                Layout.fillWidth: true
                                text: grid.soundText(row.modelData)
                                color: row.modelData.state === "failed" ? theme.colors.red : theme.colors.muted
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.chooseSample(row.index)
                                }
                            }

                            // The track's volume.
                            Rectangle {
                                implicitWidth: 44
                                implicitHeight: 4
                                radius: 2
                                color: grid.alpha(theme.colors.muted, 0.4)
                                Rectangle {
                                    width: parent.width * row.modelData.volume
                                    height: parent.height
                                    radius: 2
                                    color: theme.foreground
                                }
                            }
                        }
                    }
                }

                Item {
                    anchors.left: header.right
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    height: parent.height

                    StepArea {
                        id: steps

                        Repeater {
                            model: 16

                            delegate: Rectangle {
                                id: cell
                                required property int index
                                readonly property var step: backend.grid[row.track][index]
                                readonly property bool on: step.kind === 1
                                readonly property bool hold: step.kind === 2
                                readonly property bool bass: row.modelData.id === "bass"
                                readonly property bool playhead: backend.playing && backend.playStep === index
                                                                 && backend.playPattern === backend.editPattern
                                readonly property bool cursor: row.current && grid.cursorStep === index

                                objectName: "cell" + row.track + "_" + index
                                x: steps.cellX(index)
                                width: steps.cellWidth
                                height: parent.height
                                radius: Math.min(4, theme.radius)
                                color: on ? grid.alpha(theme.accent, bass ? 1 : [0, 0.4, 0.6, 0.8, 1][step.level])
                                     : hold ? grid.alpha(theme.accent, 0.3)
                                     : grid.alpha(theme.colors.selection, Math.floor(index / 4) % 2 === 0 ? 0.9 : 0.55)
                                border.width: cursor ? 2 : 0
                                border.color: theme.foreground

                                Rectangle {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    color: grid.alpha(theme.foreground, 0.22)
                                    visible: cell.playhead
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: cell.bass && (cell.on || cell.hold)
                                    text: cell.on ? cell.step.note : "–"
                                    color: cell.on ? theme.accentForeground : theme.foreground
                                    font.pixelSize: Math.min(12, cell.width / 3)
                                    font.bold: true
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: (mouse) => {
                                        grid.cellPicked(row.track, cell.index)
                                        if (mouse.button === Qt.RightButton && cell.bass)
                                            backend.holdStep(row.track, cell.index)
                                        else if (mouse.button === Qt.RightButton)
                                            backend.clearStep(row.track, cell.index)
                                        else
                                            backend.toggleStep(row.track, cell.index)
                                    }
                                    onWheel: (wheel) => {
                                        grid.cellPicked(row.track, cell.index)
                                        backend.changeStep(row.track, cell.index, wheel.angleDelta.y > 0 ? 1 : -1)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
