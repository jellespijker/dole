import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Names.js" as Names

// The pattern being edited, a bar at a time: a row for each pad of the
// bank, with its steps, and under a track opened up, its lanes.
Item {
    id: grid

    property int cursorStep: 0
    // -1 is the steps; 0 and up the selected track's lanes.
    property int cursorLane: -1
    // Opened tracks, by pad.
    property var expanded: ({})
    property bool active: true

    signal cellPicked(int pad, int step)
    signal lanePicked(int pad, int lane, int step)
    signal expandToggled(int pad)
    signal addLaneRequested(int pad)

    readonly property int bankStart: backend.bank * 16
    readonly property int viewBar: Math.floor(cursorStep / 16)
    readonly property int viewStart: viewBar * 16
    readonly property int bars: Math.max(1, backend.patternLength / 16)
    readonly property int headerWidth: 150
    readonly property int rulerHeight: 16
    readonly property int rowSpacing: 3
    readonly property int laneHeight: 46
    readonly property real rowHeight: Math.max(20, Math.min(36, (rows.height - 15 * rowSpacing) / 16))

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function number(n) { return n.toLocaleString(Qt.locale(translator.language), 'f', 0) }

    // Keeps the row with the cursor in view as lanes open below the rows.
    function reveal(item) {
        if (!item)
            return
        const top = item.mapToItem(column, 0, 0).y
        if (top < rows.contentY)
            rows.contentY = top
        else if (top + item.height > rows.contentY + rows.height)
            rows.contentY = top + item.height - rows.height
    }

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
        // The cell under an x, for a drag across a lane.
        function cellAt(x) {
            for (let i = 15; i >= 0; i--)
                if (x >= cellX(i))
                    return i
            return 0
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: grid.rowSpacing

        // Beat numbers over the steps, and which bar of the pattern this is.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: grid.rulerHeight

            Label {
                objectName: "barLabel"
                anchors.left: parent.left
                width: grid.headerWidth
                visible: grid.bars > 1
                text: qsTr("Bar %1 of %2").arg(grid.number(grid.viewBar + 1)).arg(grid.number(grid.bars))
                color: theme.accent
                font.pixelSize: 11
                font.bold: true
            }

            Item {
                anchors.fill: parent
                anchors.leftMargin: grid.headerWidth + 10

                StepArea {
                    id: ruler

                    Repeater {
                        model: 16
                        delegate: Label {
                            required property int index
                            readonly property int step: grid.viewStart + index
                            x: ruler.cellX(index)
                            width: ruler.cellWidth
                            horizontalAlignment: Text.AlignHCenter
                            text: index % 4 === 0 ? grid.number(index / 4 + 1) : "·"
                            color: backend.playing && backend.playStep === step && backend.playPattern === backend.editPattern
                                   ? theme.accent : grid.active && grid.cursorStep === step ? theme.foreground : theme.colors.muted
                            font.pixelSize: 11
                            font.bold: index % 4 === 0
                        }
                    }
                }
            }
        }

        Flickable {
            id: rows
            objectName: "rows"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: column.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Column {
                id: column
                width: rows.width
                spacing: grid.rowSpacing

                Repeater {
                    model: backend.grid

                    delegate: Column {
                        id: row
                        required property int index
                        required property var modelData
                        readonly property int pad: grid.bankStart + index
                        readonly property var info: backend.pads[pad]
                        readonly property bool current: backend.selectedPad === pad
                        readonly property bool open: !!grid.expanded[pad]
                        // Where this track is in its own loop as the pattern plays.
                        readonly property int playing: !backend.playing || backend.playPattern !== backend.editPattern ? -1
                                                       : modelData.loop < backend.patternLength ? backend.playTick % modelData.loop
                                                       : backend.playStep

                        width: column.width
                        spacing: grid.rowSpacing

                        onCurrentChanged: if (current && grid.cursorLane < 0) Qt.callLater(grid.reveal, stepRow)

                        Item {
                            id: stepRow
                            width: parent.width
                            height: grid.rowHeight
                            opacity: row.info.audible ? 1 : 0.45

                            Rectangle {
                                id: header
                                objectName: "padHeader" + row.pad
                                anchors.left: parent.left
                                width: grid.headerWidth
                                height: parent.height
                                radius: Math.min(4, theme.radius)
                                color: row.current && grid.cursorLane < 0 ? grid.alpha(theme.colors.selection, 0.8)
                                                                          : grid.alpha(theme.colors.selection, 0.25)

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: backend.selectPad(row.pad)
                                    onWheel: (wheel) => backend.changeVolume(row.pad, wheel.angleDelta.y > 0 ? 0.05 : -0.05)
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 2
                                    anchors.rightMargin: 4
                                    spacing: 3

                                    // Opens the track to its lanes.
                                    Label {
                                        objectName: "expand" + row.pad
                                        Layout.preferredWidth: 18
                                        Layout.fillHeight: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        text: row.open ? "▾" : "▸"
                                        color: row.modelData.lanes.length > 0 ? theme.accent : theme.colors.muted
                                        font.pixelSize: 12
                                        TapHandler { onTapped: grid.expandToggled(row.pad) }
                                    }
                                    Label {
                                        Layout.preferredWidth: 24
                                        text: row.info.name
                                        color: row.current ? theme.accent : theme.colors.muted
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: Names.pad(row.info)
                                        color: row.info.state === "failed" ? theme.colors.red
                                             : row.info.state === "empty" ? theme.colors.muted : theme.foreground
                                        font.pixelSize: 12
                                        font.bold: row.current
                                        elide: Text.ElideRight
                                    }

                                    Repeater {
                                        model: [
                                            { key: "M", on: row.info.mute, color: theme.colors.red },
                                            { key: "S", on: row.info.solo, color: theme.colors.yellow }
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
                                                onClicked: parent.index === 0 ? backend.toggleMute(row.pad) : backend.toggleSolo(row.pad)
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
                                            readonly property int step: grid.viewStart + index
                                            readonly property int level: row.modelData.steps[step] || 0
                                            readonly property int roll: row.modelData.roll[step] || 1
                                            readonly property int chance: row.modelData.chance[step] === undefined ? 100 : row.modelData.chance[step]
                                            readonly property int nudge: row.modelData.nudge[step] || 0
                                            readonly property bool outside: step >= row.modelData.loop
                                            readonly property bool playhead: row.playing === step
                                            readonly property bool cursor: grid.active && row.current && grid.cursorLane < 0
                                                                           && grid.cursorStep === step

                                            objectName: "cell" + row.pad + "_" + step
                                            x: steps.cellX(index)
                                            width: steps.cellWidth
                                            height: parent.height
                                            radius: Math.min(3, theme.radius)
                                            // Past the track's own loop, the steps are kept but not played.
                                            opacity: outside ? 0.3 : 1
                                            color: level > 0 ? grid.alpha(theme.accent, [0, 0.4, 0.6, 0.8, 1][level] * (chance < 100 ? 0.7 : 1))
                                                 : grid.alpha(theme.colors.selection, Math.floor(index / 4) % 2 === 0 ? 0.9 : 0.55)
                                            border.width: cursor ? 2 : 0
                                            border.color: theme.foreground

                                            Rectangle {
                                                anchors.fill: parent
                                                radius: parent.radius
                                                color: grid.alpha(theme.foreground, 0.22)
                                                visible: cell.playhead
                                            }
                                            // A roll: a dot a hit.
                                            Row {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                anchors.bottom: parent.bottom
                                                anchors.bottomMargin: 3
                                                spacing: 2
                                                visible: cell.level > 0 && cell.roll > 1
                                                Repeater {
                                                    model: cell.roll
                                                    Rectangle {
                                                        width: 4
                                                        height: 4
                                                        radius: 2
                                                        color: theme.accentForeground
                                                    }
                                                }
                                            }
                                            // Its chance, when it isn't every time.
                                            Label {
                                                anchors.right: parent.right
                                                anchors.top: parent.top
                                                anchors.margins: 2
                                                visible: cell.level > 0 && cell.chance < 100 && parent.width > 22
                                                text: grid.number(cell.chance) + "%"
                                                color: theme.accentForeground
                                                font.pixelSize: 8
                                                font.bold: true
                                            }
                                            // A nudge: a mark early or late of the middle.
                                            Rectangle {
                                                visible: cell.level > 0 && cell.nudge !== 0
                                                x: Math.max(0, Math.min(parent.width - 3, parent.width / 2 + cell.nudge / 100 * parent.width - 1))
                                                anchors.top: parent.top
                                                width: 3
                                                height: Math.min(8, parent.height / 3)
                                                color: theme.accentForeground
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                                onClicked: (mouse) => {
                                                    grid.cellPicked(row.pad, cell.step)
                                                    if (mouse.button === Qt.RightButton)
                                                        backend.clearStep(row.pad, cell.step)
                                                    else
                                                        backend.toggleStep(row.pad, cell.step)
                                                }
                                                onWheel: (wheel) => {
                                                    grid.cellPicked(row.pad, cell.step)
                                                    backend.changeStep(row.pad, cell.step, wheel.angleDelta.y > 0 ? 1 : -1)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // The track opened up: a lane for each setting it moves.
                        Repeater {
                            model: row.open ? row.modelData.lanes : []

                            delegate: Item {
                                id: lane
                                required property int index
                                required property var modelData
                                readonly property bool chosen: row.current && grid.cursorLane === index
                                readonly property real min: modelData.min
                                readonly property real max: modelData.max
                                readonly property real span: Math.max(1e-9, max - min)
                                // The value each step plays with: the last one set.
                                readonly property var carried: {
                                    const out = []
                                    let last = null
                                    for (let i = 0; i < modelData.values.length; i++) {
                                        if (modelData.values[i] !== null && modelData.values[i] !== undefined)
                                            last = modelData.values[i]
                                        out.push(last)
                                    }
                                    return out
                                }

                                objectName: "lane" + row.pad + "_" + modelData.id
                                width: row.width
                                height: grid.laneHeight

                                onChosenChanged: if (chosen) Qt.callLater(grid.reveal, lane)

                                Rectangle {
                                    id: laneHeader
                                    anchors.left: parent.left
                                    width: grid.headerWidth
                                    height: parent.height
                                    radius: Math.min(4, theme.radius)
                                    color: lane.chosen ? grid.alpha(theme.colors.selection, 0.8) : grid.alpha(theme.colors.selection, 0.12)

                                    TapHandler {
                                        onTapped: {
                                            backend.selectPad(row.pad)
                                            grid.lanePicked(row.pad, lane.index, grid.cursorStep)
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 22
                                        anchors.rightMargin: 4
                                        spacing: 3
                                        Label {
                                            Layout.fillWidth: true
                                            text: lane.modelData.name
                                            color: lane.chosen ? theme.foreground : theme.colors.muted
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 2
                                        }
                                        Label {
                                            text: "✕"
                                            color: theme.colors.muted
                                            font.pixelSize: 11
                                            TapHandler { onTapped: backend.removeLane(row.pad, lane.modelData.id) }
                                        }
                                    }
                                }

                                Item {
                                    anchors.left: laneHeader.right
                                    anchors.leftMargin: 10
                                    anchors.right: parent.right
                                    height: parent.height

                                    StepArea {
                                        id: laneSteps

                                        Repeater {
                                            model: 16
                                            delegate: Rectangle {
                                                id: point
                                                required property int index
                                                readonly property int step: grid.viewStart + index
                                                readonly property var set: lane.modelData.values[step]
                                                readonly property var value: lane.carried[step]
                                                readonly property bool has: value !== null && value !== undefined
                                                readonly property bool here: set !== null && set !== undefined
                                                // A range across zero, as pan's, draws from the middle.
                                                readonly property real zero: lane.min < 0 && lane.max > 0 ? (0 - lane.min) / lane.span : 0
                                                readonly property real at: has ? (value - lane.min) / lane.span : 0

                                                x: laneSteps.cellX(index)
                                                width: laneSteps.cellWidth
                                                height: parent.height
                                                radius: Math.min(3, theme.radius)
                                                color: grid.alpha(theme.colors.selection, 0.35)
                                                opacity: step >= row.modelData.loop ? 0.3 : 1
                                                border.width: lane.chosen && grid.active && grid.cursorStep === step ? 2 : 0
                                                border.color: theme.foreground

                                                Rectangle {
                                                    visible: point.has
                                                    x: 2
                                                    width: parent.width - 4
                                                    y: parent.height * (1 - Math.max(point.at, point.zero))
                                                    height: Math.max(2, parent.height * Math.abs(point.at - point.zero))
                                                    radius: Math.min(2, theme.radius)
                                                    color: grid.alpha(theme.accent, point.here ? 1 : 0.35)
                                                }
                                            }
                                        }

                                        // Drawn with a finger or the mouse; a right click or a
                                        // long press clears a step, so the one before carries on.
                                        MouseArea {
                                            anchors.fill: parent
                                            preventStealing: true
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            pressAndHoldInterval: 600
                                            property bool clearing: false
                                            function draw(mouse) {
                                                const step = grid.viewStart + laneSteps.cellAt(mouse.x)
                                                const fraction = 1 - Math.max(0, Math.min(1, mouse.y / height))
                                                backend.selectPad(row.pad)
                                                grid.lanePicked(row.pad, lane.index, step)
                                                if (clearing)
                                                    backend.clearLanePoint(row.pad, lane.modelData.id, step)
                                                else
                                                    backend.setLanePoint(row.pad, lane.modelData.id, step,
                                                                         lane.min + fraction * lane.span)
                                            }
                                            onPressed: (mouse) => {
                                                clearing = mouse.button === Qt.RightButton
                                                draw(mouse)
                                            }
                                            onPositionChanged: (mouse) => { if (pressed) draw(mouse) }
                                            onPressAndHold: (mouse) => {
                                                clearing = true
                                                draw(mouse)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Item {
                            visible: row.open
                            width: row.width
                            height: visible ? 30 : 0
                            Chip {
                                objectName: "addLane" + row.pad
                                anchors.left: parent.left
                                anchors.leftMargin: 22
                                anchors.verticalCenter: parent.verticalCenter
                                implicitHeight: 26
                                text: qsTr("Add a lane…")
                                bold: false
                                onClicked: {
                                    backend.selectPad(row.pad)
                                    grid.addLaneRequested(row.pad)
                                }
                            }
                        }
                    }
                }
            }
        }

        // The step under the cursor: how many hits, how often, and how far
        // off the grid. Big enough for a finger.
        RowLayout {
            id: stepBar
            objectName: "stepBar"
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            spacing: 6
            readonly property var gridRow: backend.grid[backend.selectedPad - grid.bankStart]
            readonly property int level: gridRow ? gridRow.steps[grid.cursorStep] || 0 : 0
            readonly property int roll: gridRow ? gridRow.roll[grid.cursorStep] || 1 : 1
            readonly property int chance: gridRow && gridRow.chance[grid.cursorStep] !== undefined ? gridRow.chance[grid.cursorStep] : 100
            readonly property int nudge: gridRow ? gridRow.nudge[grid.cursorStep] || 0 : 0

            Label {
                Layout.preferredWidth: grid.headerWidth
                text: qsTr("Step %1").arg(grid.number(grid.cursorStep + 1))
                color: stepBar.level > 0 ? theme.foreground : theme.colors.muted
                font.bold: true
            }
            Chip {
                objectName: "rollChip"
                enabled: stepBar.level > 0
                text: qsTr("Roll ×%1").arg(grid.number(stepBar.roll))
                on: stepBar.roll > 1
                bold: false
                onClicked: backend.cycleRoll(backend.selectedPad, grid.cursorStep)
            }
            Chip {
                objectName: "chanceChip"
                enabled: stepBar.level > 0
                text: qsTr("Chance %1%").arg(grid.number(stepBar.chance))
                on: stepBar.chance < 100
                bold: false
                onClicked: backend.cycleChance(backend.selectedPad, grid.cursorStep)
            }
            Label {
                Layout.leftMargin: 6
                text: qsTr("Nudge")
                color: theme.colors.muted
            }
            Chip {
                enabled: stepBar.level > 0
                text: "←"
                onClicked: backend.nudgeStep(backend.selectedPad, grid.cursorStep, -5)
            }
            Label {
                objectName: "nudge"
                Layout.minimumWidth: 44
                horizontalAlignment: Text.AlignHCenter
                text: (stepBar.nudge > 0 ? "+" : "") + grid.number(stepBar.nudge) + "%"
                color: stepBar.nudge !== 0 ? theme.foreground : theme.colors.muted
            }
            Chip {
                enabled: stepBar.level > 0
                text: "→"
                onClicked: backend.nudgeStep(backend.selectedPad, grid.cursorStep, 5)
            }
            Item { Layout.fillWidth: true }
        }
    }
}
