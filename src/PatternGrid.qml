import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Names.js" as Names

// The pattern being edited, a bar at a time, set like a photocopied flyer: a
// row for each pad of the bank, with its steps, and under a track opened up,
// its lanes. The playhead is the copier's light bar sweeping across, and the
// hits it has passed turn to toner.
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
    readonly property bool playingHere: backend.playing && backend.playPattern === backend.editPattern
    // How long a step lasts at the tempo, for the light bar's sweep.
    readonly property real stepMs: 15000 / Math.max(1, backend.bpm)
    readonly property int headerWidth: width < 640 ? 150 : 158
    readonly property int stepsLeft: headerWidth + 8
    readonly property int rowSpacing: 5
    // Lanes are drawn with a finger: tall enough to see a value under one,
    // and the lane being worked on taller still.
    readonly property int laneHeight: 64
    readonly property int laneOpenHeight: 140
    // A finger moving this far changes a lane from its bottom to its top,
    // much further than the lane is tall, so a value can be set exactly.
    readonly property int laneFineTravel: 360
    readonly property real rowHeight: Math.max(20, Math.min(36, (rows.height - 15 * rowSpacing) / 16))

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function number(n) { return n.toLocaleString(Qt.locale(translator.language), 'f', 0) }
    // A lane's value as it reads: whole steps (pitch's semitones), a percent
    // for the pad's own settings, or a plugin's own number.
    function laneText(lane, value) {
        if (value === null || value === undefined)
            return "–"
        const signed = lane.min < 0
        let shown
        if (lane.stepped)
            shown = Math.round(value).toLocaleString(Qt.locale(translator.language), 'f', 0)
        else if (lane.span <= 2)
            shown = Math.round(value * 100).toLocaleString(Qt.locale(translator.language), 'f', 0)
        else
            shown = value.toLocaleString(Qt.locale(translator.language), 'f', lane.span >= 20 ? 0 : 2)
        return (signed && value > 0 ? "+" : "") + shown
    }
    // A hit's ink by its level: full accent at the top, thinner below.
    function hitColor(level) {
        return level >= 4 ? theme.accent : Qt.tint(theme.background, alpha(theme.accent, [0, 0.45, 0.65, 0.82][level]))
    }

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
        readonly property real gap: 4
        readonly property real cellWidth: (width - 15 * gap) / 16
        function cellX(step) { return step * (cellWidth + gap) }
        // The cell under an x, for a drag across a lane.
        function cellAt(x) {
            for (let i = 15; i >= 0; i--)
                if (x >= cellX(i))
                    return i
            return 0
        }
    }

    // The tiny caps that label things, beside the bold ones that name them.
    component Caption: Label {
        font.pixelSize: 11
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.9
        color: theme.quiet
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Beat numbers over the steps, and which bar of the pattern this is.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 30

            Caption {
                objectName: "barLabel"
                anchors.left: parent.left
                anchors.leftMargin: 4
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                width: grid.headerWidth
                text: grid.bars > 1 ? qsTr("Bar %1 of %2").arg(grid.number(grid.viewBar + 1)).arg(grid.number(grid.bars))
                                    : qsTr("Step %1").arg(grid.number(grid.cursorStep + 1))
            }

            Item {
                anchors.fill: parent
                anchors.leftMargin: grid.stepsLeft

                StepArea {
                    id: ruler

                    Repeater {
                        model: 16
                        delegate: Label {
                            required property int index
                            readonly property int step: grid.viewStart + index
                            readonly property bool beat: index % 4 === 0
                            x: ruler.cellX(index)
                            width: ruler.cellWidth
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 4
                            horizontalAlignment: Text.AlignHCenter
                            text: beat ? grid.number(index / 4 + 1) : "·"
                            font.family: beat ? theme.displayFont : theme.typeFont
                            font.pixelSize: beat ? 18 : 12
                            color: grid.playingHere && backend.playStep === step ? theme.accent
                                 : grid.active && grid.cursorStep === step ? theme.foreground
                                 : beat ? theme.foreground : theme.quiet
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Flickable {
                id: rows
                objectName: "rows"
                anchors.fill: parent
                clip: true
                contentHeight: column.height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                // Drawing on the lanes, with a finger or the mouse. It lives here,
                // over every row, not in a lane: each change redraws the lanes,
                // and a handler in one would let go of the finger.
                //
                // Pressing a step and moving up or down nudges its value from
                // where it was, a lane's whole range over a long way, so a
                // finger can set it exactly; sliding across the steps draws
                // them where the finger is. A long press or a right click
                // clears a step, so the one before carries on. Anywhere but a
                // lane's steps, the press goes on to what is under it.
                MouseArea {
                    id: laneTouch
                    objectName: "laneTouch"
                    z: 10
                    width: column.width
                    height: column.height
                    preventStealing: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    pressAndHoldInterval: 600
                    property var gesture: null

                    function laneUnder(x, y) {
                        let item = column
                        while (item) {
                            if (item.isLane)
                                return item
                            const p = laneTouch.mapToItem(item, x, y)
                            item = item.childAt(p.x, p.y)
                        }
                        return null
                    }
                    function findLane(pad, id) {
                        const search = (item) => {
                            if (item.isLane && item.pad === pad && item.laneId === id)
                                return item
                            for (let i = 0; i < item.children.length; i++) {
                                const found = search(item.children[i])
                                if (found)
                                    return found
                            }
                            return null
                        }
                        return search(column)
                    }
                    function stepUnder(lane, x) {
                        const p = laneTouch.mapToItem(lane.steps, x, 0)
                        return grid.viewStart + lane.steps.cellAt(p.x)
                    }

                    onPressed: (mouse) => {
                        const lane = laneUnder(mouse.x, mouse.y)
                        const p = lane ? mapToItem(lane.steps, mouse.x, mouse.y) : null
                        if (!lane || p.x < 0 || p.x > lane.steps.width) {
                            mouse.accepted = false
                            return
                        }
                        const step = stepUnder(lane, mouse.x)
                        gesture = { pad: lane.pad, id: lane.laneId, index: lane.index, step: step,
                                    x: mouse.x, y: mouse.y, min: lane.min, span: lane.span, stepped: lane.stepped,
                                    mode: "nudge", start: undefined, clearing: mouse.button === Qt.RightButton }
                        backend.selectPad(lane.pad)
                        grid.lanePicked(lane.pad, lane.index, step)
                        if (gesture.clearing)
                            backend.clearLanePoint(lane.pad, lane.laneId, step)
                    }
                    onPositionChanged: (mouse) => {
                        const g = gesture
                        if (!g || !pressed)
                            return
                        const lane = findLane(g.pad, g.id)
                        if (!lane)
                            return
                        const step = stepUnder(lane, mouse.x)
                        if (g.clearing) {
                            if (step !== g.step) {
                                g.step = step
                                backend.clearLanePoint(g.pad, g.id, step)
                                grid.lanePicked(g.pad, g.index, step)
                            }
                            return
                        }
                        const p = mapToItem(lane.steps, mouse.x, mouse.y)
                        const drawn = g.min + (1 - Math.max(0, Math.min(1, p.y / lane.steps.height))) * g.span
                        if (g.mode === "nudge" && step !== g.step && Math.abs(mouse.x - g.x) > 16) {
                            // A slide across draws, from the step it started on.
                            g.mode = "draw"
                            backend.setLanePoint(g.pad, g.id, g.step, drawn)
                        }
                        if (g.mode === "draw") {
                            backend.setLanePoint(g.pad, g.id, step, drawn)
                            grid.lanePicked(g.pad, g.index, step)
                            return
                        }
                        const rise = g.y - mouse.y
                        if (g.start === undefined) {
                            // A touch that doesn't move only picks the step.
                            if (Math.abs(rise) < 4)
                                return
                            // From the value the step plays with now: its own, the
                            // one carried from before, or the pad's setting.
                            backend.changeLanePoint(g.pad, g.id, g.step, 0)
                            const fresh = findLane(g.pad, g.id)
                            g.start = fresh ? fresh.modelData.values[g.step] : g.min
                            g.y = mouse.y
                            return
                        }
                        backend.setLanePoint(g.pad, g.id, g.step, g.start + rise / grid.laneFineTravel * g.span)
                    }
                    onPressAndHold: {
                        const g = gesture
                        if (!g || g.start !== undefined || g.mode === "draw")
                            return
                        g.clearing = true
                        backend.clearLanePoint(g.pad, g.id, g.step)
                    }
                    onReleased: gesture = null
                    onCanceled: gesture = null
                    // The wheel nudges the step under it a hundredth of the way,
                    // or a whole step of a stepped lane.
                    onWheel: (wheel) => {
                        const lane = laneUnder(wheel.x, wheel.y)
                        const p = lane ? mapToItem(lane.steps, wheel.x, wheel.y) : null
                        if (!lane || p.x < 0 || p.x > lane.steps.width || wheel.angleDelta.y === 0) {
                            wheel.accepted = false
                            return
                        }
                        const step = stepUnder(lane, wheel.x)
                        backend.selectPad(lane.pad)
                        grid.lanePicked(lane.pad, lane.index, step)
                        backend.changeLanePoint(lane.pad, lane.laneId, step, wheel.angleDelta.y > 0 ? 0.01 : -0.01)
                    }
                }

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
                            readonly property bool ownLoop: modelData.loop < backend.patternLength
                            // Where this track is in its own loop as the pattern plays.
                            readonly property int playing: !grid.playingHere ? -1
                                                           : ownLoop ? backend.playTick % modelData.loop
                                                           : backend.playStep
                            // A gated pad or a synth holds each hit for its note length.
                            readonly property int hold: info.gate || info.synth !== "" ? Math.max(1, info.length) : 1

                            width: column.width
                            spacing: grid.rowSpacing

                            onCurrentChanged: if (current && grid.cursorLane < 0) Qt.callLater(grid.reveal, stepRow)

                            Item {
                                id: stepRow
                                width: parent.width
                                height: grid.rowHeight

                                Rectangle {
                                    id: header
                                    objectName: "padHeader" + row.pad
                                    readonly property bool inverse: row.current && grid.cursorLane < 0
                                    anchors.left: parent.left
                                    width: grid.headerWidth
                                    height: parent.height
                                    color: inverse ? theme.foreground : "transparent"

                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: backend.selectPad(row.pad)
                                        onWheel: (wheel) => backend.changeVolume(row.pad, wheel.angleDelta.y > 0 ? 0.05 : -0.05)
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 2
                                        anchors.rightMargin: 4
                                        spacing: 4

                                        // Opens the track to its lanes.
                                        Item {
                                            objectName: "expand" + row.pad
                                            Layout.preferredWidth: 22
                                            Layout.fillHeight: true
                                            Icon {
                                                anchors.centerIn: parent
                                                width: 9
                                                height: 9
                                                name: row.open ? "open" : "closed"
                                                color: header.inverse ? theme.background
                                                     : row.modelData.lanes.length > 0 ? theme.accent : theme.quiet
                                            }
                                            TapHandler { onTapped: grid.expandToggled(row.pad) }
                                        }
                                        Label {
                                            visible: grid.width >= 640
                                            Layout.preferredWidth: 28
                                            text: row.info.name
                                            color: header.inverse ? theme.background : theme.quiet
                                            font.pixelSize: 12
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: Names.pad(row.info)
                                            color: header.inverse ? theme.background
                                                 : row.info.state === "failed" ? theme.colors.red
                                                 : row.info.state === "empty" ? theme.quiet : theme.foreground
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.capitalization: Font.AllUppercase
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
                                                readonly property color ink: header.inverse ? theme.background : theme.quiet
                                                implicitWidth: 18
                                                implicitHeight: 16
                                                color: modelData.on ? modelData.color : "transparent"
                                                border.width: modelData.on ? 0 : 1.5
                                                border.color: ink
                                                Label {
                                                    anchors.centerIn: parent
                                                    text: parent.modelData.key
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: parent.modelData.on ? theme.background : parent.ink
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    anchors.margins: -4
                                                    onClicked: parent.index === 0 ? backend.toggleMute(row.pad) : backend.toggleSolo(row.pad)
                                                }
                                            }
                                        }
                                    }
                                }

                                Item {
                                    anchors.left: header.right
                                    anchors.leftMargin: 8
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
                                                readonly property bool cursor: grid.active && row.current && grid.cursorLane < 0
                                                                               && grid.cursorStep === step
                                                // Toner: the light bar has passed this hit in this loop.
                                                readonly property bool printed: level > 0 && row.playing > step
                                                readonly property color ink: printed ? theme.foreground : grid.hitColor(level)
                                                // How many steps the hit holds for, up to the next hit.
                                                readonly property int span: {
                                                    if (level === 0 || row.hold <= 1)
                                                        return 1
                                                    let n = 1
                                                    while (n < row.hold && index + n < 16 && !(row.modelData.steps[step + n] > 0))
                                                        n++
                                                    return n
                                                }

                                                objectName: "cell" + row.pad + "_" + step
                                                x: steps.cellX(index)
                                                z: span > 1 ? 1 : 0
                                                width: span > 1 ? steps.cellX(index + span - 1) + steps.cellWidth - x : steps.cellWidth
                                                height: parent.height
                                                // Past the track's own loop, the steps are kept but not played.
                                                opacity: outside ? 0.3 : 1
                                                color: level > 0 ? ink : "transparent"
                                                border.width: level > 0 ? 0 : 1.5
                                                border.color: grid.alpha(theme.foreground, 0.28)

                                                // The toner fusing: as the light bar passes a hit, its
                                                // ink spreads and fades, so a step that sounded shows it.
                                                onPrintedChanged: if (printed && theme.motion) fuse.restart()
                                                Rectangle {
                                                    id: toner
                                                    anchors.fill: parent
                                                    color: theme.accent
                                                    opacity: 0
                                                    ParallelAnimation {
                                                        id: fuse
                                                        NumberAnimation { target: toner; property: "opacity"; from: 0.85; to: 0; duration: 200; easing.type: Easing.OutCubic }
                                                        NumberAnimation { target: toner; property: "scale"; from: 1; to: 1.3; duration: 200; easing.type: Easing.OutCubic }
                                                    }
                                                }
                                                // Each beat starts on a heavier rule.
                                                Rectangle {
                                                    visible: cell.index % 4 === 0 && cell.level === 0
                                                    width: 3
                                                    height: parent.height
                                                    color: grid.alpha(theme.foreground, 0.55)
                                                }
                                                // A held note ends on a solid cap.
                                                Rectangle {
                                                    visible: cell.span > 1
                                                    anchors.right: parent.right
                                                    width: 6
                                                    height: parent.height
                                                    color: cell.printed ? theme.accent : theme.foreground
                                                }
                                                // Where a track with its own loop is, under the light bar's sweep.
                                                Rectangle {
                                                    anchors.fill: parent
                                                    visible: row.ownLoop && row.playing === cell.step
                                                    color: grid.alpha(theme.foreground, 0.3)
                                                }
                                                // A roll: the step cut into its hits.
                                                Repeater {
                                                    model: cell.level > 0 ? cell.roll - 1 : 0
                                                    Rectangle {
                                                        required property int index
                                                        x: steps.cellWidth * (index + 1) / cell.roll - 1
                                                        width: 2
                                                        height: cell.height
                                                        color: theme.background
                                                    }
                                                }
                                                // Its chance, when it isn't every time.
                                                Label {
                                                    anchors.right: parent.right
                                                    anchors.top: parent.top
                                                    anchors.rightMargin: cell.span > 1 ? 9 : 2
                                                    anchors.topMargin: 1
                                                    visible: cell.level > 0 && cell.chance < 100 && steps.cellWidth > 30
                                                    text: grid.number(cell.chance) + "%"
                                                    color: cell.printed ? theme.background : theme.accentForeground
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                }
                                                // A nudge: a mark early or late of the middle.
                                                Rectangle {
                                                    visible: cell.level > 0 && cell.nudge !== 0
                                                    x: Math.max(0, Math.min(steps.cellWidth - 3, steps.cellWidth / 2 + cell.nudge / 100 * steps.cellWidth - 1))
                                                    anchors.bottom: parent.bottom
                                                    width: 3
                                                    height: Math.min(9, parent.height / 3)
                                                    color: cell.printed ? theme.background : theme.accentForeground
                                                }
                                                // The cursor: a heavy rule around the step.
                                                Rectangle {
                                                    anchors.fill: parent
                                                    anchors.margins: -4
                                                    visible: cell.cursor
                                                    color: "transparent"
                                                    border.width: 3
                                                    border.color: theme.foreground
                                                }

                                                MouseArea {
                                                    width: steps.cellWidth
                                                    height: parent.height
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

                                    // A track that isn't heard is struck through, and a mute
                                    // stamped across it: never faded out.
                                    Rectangle {
                                        visible: !row.info.audible
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width
                                        height: 3
                                        z: 2
                                        color: row.info.mute ? theme.colors.red : theme.quiet
                                    }
                                    Stamp {
                                        visible: row.info.mute
                                        z: 3
                                        x: parent.width * 0.3
                                        anchors.verticalCenter: parent.verticalCenter
                                        size: Math.max(14, Math.min(24, grid.rowHeight * 0.7))
                                        text: qsTr("Mute")
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

                                    // Found by the touch over the grid, which outlives this
                                    // lane as each change redraws it.
                                    readonly property bool isLane: true
                                    readonly property int pad: row.pad
                                    readonly property string laneId: modelData.id
                                    readonly property bool stepped: modelData.stepped
                                    readonly property alias steps: laneSteps

                                    objectName: "lane" + row.pad + "_" + modelData.id
                                    width: row.width
                                    height: chosen ? grid.laneOpenHeight : grid.laneHeight

                                    onChosenChanged: if (chosen) Qt.callLater(grid.reveal, lane)

                                    Rectangle {
                                        id: laneHeader
                                        anchors.left: parent.left
                                        width: grid.headerWidth
                                        height: parent.height
                                        color: lane.chosen ? theme.foreground : "transparent"

                                        TapHandler {
                                            onTapped: {
                                                backend.selectPad(row.pad)
                                                grid.lanePicked(row.pad, lane.index, grid.cursorStep)
                                            }
                                        }

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 34
                                            anchors.rightMargin: 2
                                            spacing: 2
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Caption {
                                                    Layout.fillWidth: true
                                                    text: lane.modelData.name
                                                    color: lane.chosen ? theme.background : theme.foreground
                                                    elide: Text.ElideRight
                                                    wrapMode: Text.Wrap
                                                    maximumLineCount: 2
                                                }
                                                // The step being set, read out here, away from
                                                // the finger that covers it.
                                                Label {
                                                    objectName: "laneValue"
                                                    visible: lane.chosen
                                                    text: grid.laneText(lane, lane.carried[grid.cursorStep])
                                                    color: theme.background
                                                    font.family: theme.displayFont
                                                    font.pixelSize: 30
                                                }
                                            }
                                            Item {
                                                Layout.alignment: Qt.AlignTop
                                                implicitWidth: 36
                                                implicitHeight: 36
                                                Icon {
                                                    anchors.centerIn: parent
                                                    width: 11
                                                    height: 11
                                                    name: "close"
                                                    color: lane.chosen ? theme.background : theme.quiet
                                                }
                                                TapHandler { onTapped: backend.removeLane(row.pad, lane.modelData.id) }
                                            }
                                        }
                                    }

                                    Item {
                                        anchors.left: laneHeader.right
                                        anchors.leftMargin: 8
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
                                                    color: grid.alpha(theme.foreground, 0.1)
                                                    opacity: step >= row.modelData.loop ? 0.3 : 1

                                                    Rectangle {
                                                        visible: point.has
                                                        x: 3
                                                        width: parent.width - 6
                                                        y: parent.height * (1 - Math.max(point.at, point.zero))
                                                        height: Math.max(2, parent.height * Math.abs(point.at - point.zero))
                                                        color: point.here ? theme.foreground : grid.alpha(theme.foreground, 0.35)
                                                    }
                                                    Rectangle {
                                                        anchors.fill: parent
                                                        anchors.margins: -4
                                                        visible: lane.chosen && (grid.active || laneTouch.pressed) && grid.cursorStep === point.step
                                                        color: "transparent"
                                                        border.width: 3
                                                        border.color: theme.foreground
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            Item {
                                visible: row.open
                                width: row.width
                                height: visible ? 38 : 0
                                Chip {
                                    objectName: "addLane" + row.pad
                                    anchors.left: parent.left
                                    anchors.leftMargin: 34
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitHeight: 30
                                    text: qsTr("Add a lane…")
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

            // The copier's light bar: the playhead, the full height of the
            // grid, over the step being played.
            Item {
                anchors.fill: parent
                anchors.leftMargin: grid.stepsLeft
                visible: grid.playingHere && Math.floor(backend.playStep / 16) === grid.viewBar

                StepArea {
                    id: scan
                    // How far the light has crossed the step being played.
                    property real progress: 1
                    readonly property int step: backend.playStep
                    onStepChanged: if (theme.motion) sweep.restart()

                    NumberAnimation {
                        id: sweep
                        target: scan
                        property: "progress"
                        from: 0
                        to: 1
                        // Time is even: the light crosses at a constant speed.
                        duration: grid.stepMs
                    }

                    Rectangle {
                        id: band
                        x: scan.cellX(backend.playStep % 16) - 2
                        width: scan.cellWidth + 4
                        height: parent.height
                        color: grid.alpha(theme.foreground, 0.2)
                        Rectangle {
                            objectName: "lightEdge"
                            x: theme.motion ? (band.width - width) * scan.progress : band.width - width
                            width: 4
                            height: parent.height
                            color: theme.foreground
                        }
                    }
                }
            }
        }

        // The step under the cursor: how many hits, how often, and how far
        // off the grid. Big enough for a finger.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 3
            color: theme.foreground
        }
        RowLayout {
            id: stepBar
            objectName: "stepBar"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            clip: true
            Layout.preferredHeight: 52
            spacing: 8
            readonly property var gridRow: backend.grid[backend.selectedPad - grid.bankStart]
            readonly property int level: gridRow ? gridRow.steps[grid.cursorStep] || 0 : 0
            readonly property int roll: gridRow ? gridRow.roll[grid.cursorStep] || 1 : 1
            readonly property int chance: gridRow && gridRow.chance[grid.cursorStep] !== undefined ? gridRow.chance[grid.cursorStep] : 100
            readonly property int nudge: gridRow ? gridRow.nudge[grid.cursorStep] || 0 : 0

            Label {
                Layout.preferredWidth: grid.headerWidth
                Layout.minimumWidth: 70
                text: qsTr("Step %1").arg(grid.number(grid.cursorStep + 1))
                font.family: theme.displayFont
                font.pixelSize: 24
                font.capitalization: Font.AllUppercase
                color: stepBar.level > 0 ? theme.foreground : theme.quiet
            }
            Chip {
                objectName: "rollChip"
                enabled: stepBar.level > 0
                text: qsTr("Roll ×%1").arg(grid.number(stepBar.roll))
                on: stepBar.roll > 1
                onClicked: backend.cycleRoll(backend.selectedPad, grid.cursorStep)
            }
            Chip {
                objectName: "chanceChip"
                enabled: stepBar.level > 0
                text: qsTr("Chance %1%").arg(grid.number(stepBar.chance))
                on: stepBar.chance < 100
                onClicked: backend.cycleChance(backend.selectedPad, grid.cursorStep)
            }
            // Early is left and late is right in every language.
            RowLayout {
                spacing: 8
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true
                Caption {
                    Layout.leftMargin: 8
                    text: qsTr("Nudge")
                }
                Chip {
                    enabled: stepBar.level > 0
                    implicitWidth: 40
                    icon: "left"
                    onClicked: backend.nudgeStep(backend.selectedPad, grid.cursorStep, -5)
                }
                Label {
                    objectName: "nudge"
                    Layout.minimumWidth: 44
                    horizontalAlignment: Text.AlignHCenter
                    text: (stepBar.nudge > 0 ? "+" : "") + grid.number(stepBar.nudge) + "%"
                    font.bold: stepBar.nudge !== 0
                    color: stepBar.nudge !== 0 ? theme.foreground : theme.quiet
                }
                Chip {
                    enabled: stepBar.level > 0
                    implicitWidth: 40
                    icon: "right"
                    onClicked: backend.nudgeStep(backend.selectedPad, grid.cursorStep, 5)
                }
            }
            Item { Layout.fillWidth: true }
            Caption {
                visible: grid.width > 760
                text: qsTr("L lanes · T roll · Y chance")
            }
        }
    }
}
