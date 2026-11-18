import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Names.js" as Names

// The pattern being edited: a row for each pad, with its sixteen steps.
Item {
    id: grid

    property int cursorStep: 0
    property bool active: true

    signal cellPicked(int pad, int step)

    readonly property int headerWidth: 132
    readonly property int rulerHeight: 16
    readonly property int rowSpacing: 3
    readonly property real rowHeight: Math.max(18, Math.min(40, (height - rulerHeight - 16 * rowSpacing) / 16))

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // Steps are time, which reads left to right in every language, so the
    // step area places its cells by x and doesn't mirror. Its parent is
    // anchored, mirrored, beside the pad names; it fills that parent.
    component StepArea: Item {
        anchors.fill: parent
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        readonly property real gap: 3
        readonly property real groupGap: 8
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
                anchors.leftMargin: grid.headerWidth + 10

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
                                   ? theme.accent : grid.active && grid.cursorStep === index ? theme.foreground : theme.colors.muted
                            font.pixelSize: 11
                            font.bold: index % 4 === 0
                        }
                    }
                }
            }
        }

        Repeater {
            model: backend.pads

            delegate: Item {
                id: row
                required property int index
                required property var modelData
                readonly property bool current: backend.selectedPad === index

                width: grid.width
                height: grid.rowHeight
                opacity: modelData.audible ? 1 : 0.45

                Rectangle {
                    id: header
                    objectName: "padHeader" + row.index
                    anchors.left: parent.left
                    width: grid.headerWidth
                    height: parent.height
                    radius: Math.min(4, theme.radius)
                    color: row.current ? grid.alpha(theme.colors.selection, 0.8) : grid.alpha(theme.colors.selection, 0.25)

                    MouseArea {
                        anchors.fill: parent
                        onClicked: backend.selectPad(row.index)
                        onWheel: (wheel) => backend.changeVolume(row.index, wheel.angleDelta.y > 0 ? 0.05 : -0.05)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 4
                        spacing: 3

                        Label {
                            Layout.preferredWidth: 18
                            text: (row.index + 1).toLocaleString(Qt.locale(translator.language), 'f', 0)
                            color: row.current ? theme.accent : theme.colors.muted
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: Names.pad(row.modelData)
                            color: row.modelData.state === "failed" ? theme.colors.red : theme.foreground
                            font.pixelSize: 12
                            font.bold: row.current
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
                                implicitWidth: 16
                                implicitHeight: 14
                                radius: Math.min(3, theme.radius)
                                color: modelData.on ? modelData.color : "transparent"
                                border.color: modelData.on ? modelData.color : grid.alpha(theme.colors.muted, 0.6)
                                Label {
                                    anchors.centerIn: parent
                                    text: parent.modelData.key
                                    font.pixelSize: 9
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
                }

                Item {
                    anchors.left: header.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    height: parent.height

                    StepArea {
                        id: steps

                        Repeater {
                            model: 16

                            delegate: Rectangle {
                                id: cell
                                required property int index
                                readonly property int level: backend.grid[row.index][index]
                                readonly property bool playhead: backend.playing && backend.playStep === index
                                                                 && backend.playPattern === backend.editPattern
                                readonly property bool cursor: grid.active && row.current && grid.cursorStep === index

                                objectName: "cell" + row.index + "_" + index
                                x: steps.cellX(index)
                                width: steps.cellWidth
                                height: parent.height
                                radius: Math.min(3, theme.radius)
                                color: level > 0 ? grid.alpha(theme.accent, [0, 0.4, 0.6, 0.8, 1][level])
                                     : grid.alpha(theme.colors.selection, Math.floor(index / 4) % 2 === 0 ? 0.9 : 0.55)
                                border.width: cursor ? 2 : 0
                                border.color: theme.foreground

                                Rectangle {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    color: grid.alpha(theme.foreground, 0.22)
                                    visible: cell.playhead
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: (mouse) => {
                                        grid.cellPicked(row.index, cell.index)
                                        if (mouse.button === Qt.RightButton)
                                            backend.clearStep(row.index, cell.index)
                                        else
                                            backend.toggleStep(row.index, cell.index)
                                    }
                                    onWheel: (wheel) => {
                                        grid.cellPicked(row.index, cell.index)
                                        backend.changeStep(row.index, cell.index, wheel.angleDelta.y > 0 ? 1 : -1)
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
