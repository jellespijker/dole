import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Names.js" as Names

ApplicationWindow {
    id: win
    width: 1280
    height: 800
    minimumWidth: 960
    minimumHeight: 620
    visible: true
    title: (backend.fileName || qsTr("Untitled")) + (backend.dirty ? " •" : "") + " — Dole"
    color: theme.background
    font.family: theme.typeFont
    font.pixelSize: 14

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    // The heavy rule the sheet is divided by.
    component Rule: Rectangle {
        property bool vertical: false
        Layout.fillWidth: !vertical
        Layout.fillHeight: vertical
        implicitWidth: 3
        implicitHeight: 3
        color: theme.foreground
    }

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    // Keys go to the window only while no dialog is up.
    readonly property bool idle: !help.opened && !confirm.opened && !chop.opened && !chain.opened && !mixer.opened
                                 && !lanePicker.opened
    property int cursorStep: 0
    // -1 is the steps; 0 and up the selected track's lanes, when it is open.
    property int cursorLane: -1
    // The tracks opened up to their lanes, by pad.
    property var expanded: ({})
    property bool songFocus: false
    property int songCursor: 0
    property bool closing: false
    // The header's height sets its type, so it comes from the window, never
    // from the header's own size.
    readonly property int headerHeight: height < 760 ? 76 : 92
    // On a narrow window the transport keeps only its marks.
    readonly property bool compact: width < 1240

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function patternName(p) { return String.fromCharCode(65 + p) }
    function chainStart() { return Math.min(songCursor, Math.max(0, backend.chain.length - 1)) }

    function bankRow(pad) { return backend.grid[pad - backend.bank * 16] }
    function laneCount(pad) {
        const row = bankRow(pad)
        return expanded[pad] && row ? row.lanes.length : 0
    }
    function laneAt(pad, lane) { return bankRow(pad).lanes[lane].id }
    function onLane() { return cursorLane >= 0 && cursorLane < laneCount(backend.selectedPad) }

    function toggleExpanded(pad) {
        const open = Object.assign({}, expanded)
        open[pad] = !open[pad]
        expanded = open
        if (!open[pad] && pad === backend.selectedPad)
            cursorLane = -1
    }
    function expand(pad) {
        if (!expanded[pad])
            toggleExpanded(pad)
    }
    // A track's lanes open, or, with none yet, the list of what can have one.
    function showLanes(pad) {
        const row = bankRow(pad)
        if (row && row.lanes.length === 0)
            lanePicker.openFor(pad)
        else
            toggleExpanded(pad)
    }

    // Up and down go through the tracks of the bank and the lanes of the
    // open ones.
    function moveRow(delta) {
        const pad = backend.selectedPad
        const start = backend.bank * 16
        if (delta < 0) {
            if (cursorLane >= 0) {
                cursorLane--
                return
            }
            const previous = start + (pad - start + 15) % 16
            backend.selectPad(previous)
            cursorLane = laneCount(previous) - 1
        } else {
            if (cursorLane + 1 < laneCount(pad)) {
                cursorLane++
                return
            }
            backend.selectPad(start + (pad - start + 1) % 16)
            cursorLane = -1
        }
    }
    function moveStep(delta) {
        const length = backend.patternLength
        cursorStep = ((cursorStep + delta) % length + length) % length
    }

    function moveSongCursor(delta) {
        songCursor = Math.max(0, Math.min(backend.chain.length, songCursor + delta))
    }

    // Typing a pattern into the song sets the slot under the cursor, or adds
    // one at the end, and moves on.
    function typePattern(p) {
        if (songCursor >= backend.chain.length)
            backend.chainInsert(songCursor, p)
        else
            backend.chainSet(songCursor, p)
        songCursor = songCursor + 1
    }

    // The pad a key plays, by where the key sits rather than what it's
    // called, so the pads keep their shape on any keyboard layout: the rows
    // from 1 2 3 4 down to Z X C V, pad 1 at the bottom left.
    readonly property var padScanCodes: ({ 52: 0, 53: 1, 54: 2, 55: 3, 38: 4, 39: 5, 40: 6, 41: 7,
                                           24: 8, 25: 9, 26: 10, 27: 11, 10: 12, 11: 13, 12: 14, 13: 15 })
    readonly property var padKeys: ({ [Qt.Key_Z]: 0, [Qt.Key_X]: 1, [Qt.Key_C]: 2, [Qt.Key_V]: 3,
                                      [Qt.Key_A]: 4, [Qt.Key_S]: 5, [Qt.Key_D]: 6, [Qt.Key_F]: 7,
                                      [Qt.Key_Q]: 8, [Qt.Key_W]: 9, [Qt.Key_E]: 10, [Qt.Key_R]: 11,
                                      [Qt.Key_1]: 12, [Qt.Key_2]: 13, [Qt.Key_3]: 14, [Qt.Key_4]: 15,
                                      [Qt.Key_Exclam]: 12, [Qt.Key_At]: 13, [Qt.Key_NumberSign]: 14, [Qt.Key_Dollar]: 15 })
    function padFor(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return -1
        if (event.nativeScanCode in padScanCodes)
            return padScanCodes[event.nativeScanCode]
        return event.key in padKeys ? padKeys[event.key] : -1
    }

    // Runs action once the song is safe: at once if nothing is unsaved,
    // otherwise after asking.
    function guard(action) {
        if (!backend.dirty) {
            action()
            return
        }
        confirm.action = action
        confirm.open()
    }

    onClosing: (close) => {
        if (backend.dirty && !closing) {
            close.accepted = false
            guard(() => { closing = true; win.close() })
        }
    }
    // A pad held when the window loses the keyboard would repeat forever.
    onActiveChanged: if (!active) backend.releaseAll()

    Connections {
        target: backend
        function onSaved() {
            if (confirm.waiting) {
                confirm.waiting = false
                const action = confirm.action
                confirm.action = null
                action()
            }
        }
        function onSaveAborted() {
            confirm.waiting = false
            confirm.action = null
        }
        function onSongChanged() {
            if (win.songCursor > backend.chain.length)
                win.songCursor = backend.chain.length
        }
        function onSelectedPadChanged() { win.cursorLane = -1 }
        function onGridChanged() {
            if (win.cursorStep >= backend.patternLength)
                win.cursorStep = backend.patternLength - 1
            if (win.cursorLane >= win.laneCount(backend.selectedPad))
                win.cursorLane = win.laneCount(backend.selectedPad) - 1
        }
    }

    // Playing.
    Shortcut { sequence: "Space"; enabled: win.idle; onActivated: backend.togglePlay(win.chainStart()) }
    Shortcut { sequence: "Shift+Space"; enabled: win.idle; onActivated: backend.toggleRecord(win.chainStart()) }
    Shortcut { sequence: "N"; enabled: win.idle; onActivated: backend.toggleRepeat() }
    Shortcut { sequence: "Shift+N"; enabled: win.idle; onActivated: backend.cycleRepeat() }
    Shortcut { sequence: "P"; enabled: win.idle; onActivated: backend.toggleSongMode() }
    Shortcut { sequence: "]"; enabled: win.idle; onActivated: backend.changeBpm(1) }
    Shortcut { sequence: "["; enabled: win.idle; onActivated: backend.changeBpm(-1) }
    Shortcut { sequence: "}"; enabled: win.idle; onActivated: backend.changeBpm(10) }
    Shortcut { sequence: "{"; enabled: win.idle; onActivated: backend.changeBpm(-10) }
    Shortcut { sequence: "."; enabled: win.idle; onActivated: backend.changeSwing(1) }
    Shortcut { sequence: ","; enabled: win.idle; onActivated: backend.changeSwing(-1) }
    Shortcut { sequence: "G"; enabled: win.idle; onActivated: backend.cycleGrit() }
    Shortcut { sequence: "Shift+G"; enabled: win.idle; onActivated: backend.toggleVinyl() }
    Shortcut { sequence: "Tab"; enabled: win.idle; onActivated: win.songFocus = !win.songFocus }

    // The pad chosen.
    Shortcut { sequence: "M"; enabled: win.idle; onActivated: backend.toggleMute(backend.selectedPad) }
    Shortcut { sequence: "Shift+M"; enabled: win.idle; onActivated: backend.toggleSolo(backend.selectedPad) }
    Shortcut { sequence: "="; enabled: win.idle; onActivated: backend.changeTune(backend.selectedPad, 1) }
    Shortcut { sequence: "-"; enabled: win.idle; onActivated: backend.changeTune(backend.selectedPad, -1) }
    Shortcut { sequence: "+"; enabled: win.idle; onActivated: backend.changeTune(backend.selectedPad, 12) }
    Shortcut { sequence: "_"; enabled: win.idle; onActivated: backend.changeTune(backend.selectedPad, -12) }
    Shortcut { sequence: "Shift+Right"; enabled: win.idle; onActivated: backend.changeVolume(backend.selectedPad, 0.05) }
    Shortcut { sequence: "Shift+Left"; enabled: win.idle; onActivated: backend.changeVolume(backend.selectedPad, -0.05) }
    Shortcut { sequence: "U"; enabled: win.idle; onActivated: backend.cycleGroup(backend.selectedPad) }
    Shortcut { sequence: "O"; enabled: win.idle; onActivated: backend.chooseSample(backend.selectedPad) }
    Shortcut { sequence: "Shift+O"; enabled: win.idle; onActivated: backend.resetPad(backend.selectedPad) }
    Shortcut { sequence: "K"; enabled: win.idle; onActivated: chop.open() }
    Shortcut { sequence: "J"; enabled: win.idle; onActivated: chain.openFor(backend.selectedPad) }
    Shortcut { sequence: "Shift+J"; enabled: win.idle; onActivated: chain.openFor(-1) }
    Shortcut { sequence: "B"; enabled: win.idle; onActivated: backend.setBank((backend.bank + 1) % 4) }
    Shortcut { sequence: "Shift+B"; enabled: win.idle; onActivated: backend.setBank((backend.bank + 3) % 4) }
    Shortcut { sequence: "L"; enabled: win.idle; onActivated: win.showLanes(backend.selectedPad) }
    Shortcut { sequence: "Shift+L"; enabled: win.idle; onActivated: lanePicker.openFor(backend.selectedPad) }
    Shortcut { sequence: "Ctrl+M"; enabled: win.idle; onActivated: mixer.open() }

    // Patterns.
    Shortcut { sequence: "Ctrl+C"; enabled: win.idle; onActivated: backend.copyPattern() }
    Shortcut { sequence: "Ctrl+V"; enabled: win.idle; onActivated: backend.pastePattern() }
    Shortcut { sequence: "Ctrl+Delete"; enabled: win.idle; onActivated: backend.clearPattern() }
    Shortcut { sequence: "Ctrl+B"; enabled: win.idle; onActivated: backend.cyclePatternLength() }
    Shortcut { sequence: "PgUp"; enabled: win.idle; onActivated: backend.selectPattern(Math.max(0, backend.editPattern - 1)) }
    Shortcut { sequence: "PgDown"; enabled: win.idle; onActivated: backend.selectPattern(Math.min(7, backend.editPattern + 1)) }
    Repeater {
        model: 8
        Item {
            required property int index
            Shortcut {
                sequence: "Ctrl+" + (index + 1)
                enabled: win.idle
                onActivated: win.songFocus ? win.typePattern(index) : backend.selectPattern(index)
            }
        }
    }

    // Files and the rest.
    Shortcut { sequences: [StandardKey.Undo]; enabled: win.idle; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; enabled: win.idle; onActivated: backend.redo() }
    Shortcut { sequences: [StandardKey.New]; enabled: win.idle; onActivated: win.guard(backend.newSong) }
    Shortcut { sequences: [StandardKey.Open]; enabled: win.idle; onActivated: win.guard(backend.openSong) }
    Shortcut { sequences: [StandardKey.Save]; enabled: win.idle; onActivated: backend.save() }
    Shortcut { sequences: ["Ctrl+Shift+S"]; enabled: win.idle; onActivated: backend.saveAs() }
    Shortcut { sequence: "Ctrl+E"; enabled: win.idle; onActivated: backend.exportAudio() }
    Shortcut {
        sequence: "?"
        enabled: !confirm.opened && !chop.opened && !chain.opened && !mixer.opened && !lanePicker.opened
        onActivated: help.opened ? help.close() : help.open()
    }
    Shortcut { sequences: ["Ctrl+Q"]; enabled: win.idle; onActivated: win.close() }
    Shortcut {
        sequence: "Escape"
        enabled: win.idle
        onActivated: win.songFocus ? win.songFocus = false : backend.dismissMessage()
    }

    // The steps.
    Shortcut { sequence: "Left"; enabled: win.idle && !win.songFocus; onActivated: win.moveStep(-1) }
    Shortcut { sequence: "Right"; enabled: win.idle && !win.songFocus; onActivated: win.moveStep(1) }
    Shortcut { sequence: "Ctrl+Left"; enabled: win.idle && !win.songFocus; onActivated: win.moveStep(-16) }
    Shortcut { sequence: "Ctrl+Right"; enabled: win.idle && !win.songFocus; onActivated: win.moveStep(16) }
    Shortcut { sequence: "Up"; enabled: win.idle && !win.songFocus; onActivated: win.moveRow(-1) }
    Shortcut { sequence: "Down"; enabled: win.idle && !win.songFocus; onActivated: win.moveRow(1) }
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: win.idle && !win.songFocus && !win.onLane()
        onActivated: backend.toggleStep(backend.selectedPad, win.cursorStep)
    }
    Shortcut {
        sequences: ["Delete", "Backspace"]
        enabled: win.idle && !win.songFocus
        onActivated: win.onLane() ? backend.clearLanePoint(backend.selectedPad, win.laneAt(backend.selectedPad, win.cursorLane), win.cursorStep)
                                  : backend.clearStep(backend.selectedPad, win.cursorStep)
    }
    Shortcut {
        sequence: "Shift+Up"
        enabled: win.idle && !win.songFocus
        onActivated: win.onLane() ? backend.changeLanePoint(backend.selectedPad, win.laneAt(backend.selectedPad, win.cursorLane), win.cursorStep, 0.05)
                                  : backend.changeStep(backend.selectedPad, win.cursorStep, 1)
    }
    Shortcut {
        sequence: "Shift+Down"
        enabled: win.idle && !win.songFocus
        onActivated: win.onLane() ? backend.changeLanePoint(backend.selectedPad, win.laneAt(backend.selectedPad, win.cursorLane), win.cursorStep, -0.05)
                                  : backend.changeStep(backend.selectedPad, win.cursorStep, -1)
    }
    Shortcut { sequence: "T"; enabled: win.idle && !win.songFocus; onActivated: backend.cycleRoll(backend.selectedPad, win.cursorStep) }
    Shortcut { sequence: "Y"; enabled: win.idle && !win.songFocus; onActivated: backend.cycleChance(backend.selectedPad, win.cursorStep) }
    Shortcut { sequence: "Alt+Left"; enabled: win.idle && !win.songFocus; onActivated: backend.nudgeStep(backend.selectedPad, win.cursorStep, -5) }
    Shortcut { sequence: "Alt+Right"; enabled: win.idle && !win.songFocus; onActivated: backend.nudgeStep(backend.selectedPad, win.cursorStep, 5) }

    // The song.
    Shortcut { sequence: "Left"; enabled: win.idle && win.songFocus; onActivated: win.moveSongCursor(-1) }
    Shortcut { sequence: "Right"; enabled: win.idle && win.songFocus; onActivated: win.moveSongCursor(1) }
    Shortcut {
        sequences: ["I", "Insert"]
        enabled: win.idle && win.songFocus
        onActivated: backend.chainInsert(win.songCursor, win.songCursor < backend.chain.length
                                                         ? backend.chain[win.songCursor] : backend.editPattern)
    }
    Shortcut { sequence: "Delete"; enabled: win.idle && win.songFocus; onActivated: backend.chainRemove(win.songCursor) }
    Shortcut {
        sequence: "Backspace"
        enabled: win.idle && win.songFocus && win.songCursor > 0
        onActivated: { backend.chainRemove(win.songCursor - 1); win.songCursor-- }
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: win.idle && win.songFocus && win.songCursor < backend.chain.length
        onActivated: { backend.selectPattern(backend.chain[win.songCursor]); win.songFocus = false }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction. The pads play from the keyboard here:
    // a press strikes, a release lets go, and a key held repeats with note
    // repeat on rather than the keyboard's own repeat. Shift strikes harder.
    Item {
        id: keys
        anchors.fill: parent
        focus: true
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Keys.onPressed: (event) => {
            const pad = win.padFor(event)
            if (pad < 0 || !win.idle)
                return
            event.accepted = true
            if (!event.isAutoRepeat)
                backend.padDown(backend.bank * 16 + pad, event.modifiers & Qt.ShiftModifier ? 4 : 3)
        }
        Keys.onReleased: (event) => {
            const pad = win.padFor(event)
            if (pad < 0)
                return
            event.accepted = true
            if (!event.isAutoRepeat)
                backend.padUp(backend.bank * 16 + pad)
        }

        // The flyer: the header's top line, the pads on their stock beside
        // the pattern, and the song along the foot, ruled off heavily.
        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Transport: play, record, tempo as the headline, and the sound's grit.
            RowLayout {
                id: header
                Layout.fillWidth: true
                Layout.preferredHeight: win.headerHeight
                Layout.maximumHeight: win.headerHeight
                spacing: 0

                // As wide as the pads below it, or what the transport leaves.
                Item {
                    Layout.preferredWidth: Math.max(150, Math.min(pads.width, win.width - transport.implicitWidth - 40
                                                                  - tempo.implicitWidth - 36 - effects.implicitWidth - 22 - 9))
                    Layout.fillHeight: true
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 22
                        anchors.rightMargin: 16
                        spacing: 16
                        Label {
                            text: "dole"
                            font.family: theme.displayFont
                            font.pixelSize: win.headerHeight * 0.66
                            font.capitalization: Font.AllUppercase
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                objectName: "fileName"
                                Layout.fillWidth: true
                                text: (backend.fileName || qsTr("Untitled")) + (backend.dirty ? " •" : "")
                                elide: Text.ElideMiddle
                                font.pixelSize: 13
                                font.bold: true
                            }
                            Label {
                                objectName: "exporting"
                                visible: backend.exporting
                                text: qsTr("Exporting…")
                                color: theme.accent
                                font.pixelSize: 12
                            }
                        }
                    }
                }
                Rule { vertical: true }

                RowLayout {
                    id: transport
                    Layout.leftMargin: 20
                    Layout.rightMargin: 20
                    spacing: 10
                    Chip {
                        objectName: "play"
                        display: true
                        implicitHeight: win.headerHeight * 0.6
                        icon: backend.playing ? "stop" : "play"
                        text: win.compact ? "" : backend.playing ? qsTr("Stop") : qsTr("Play")
                        on: backend.playing
                        onColor: theme.foreground
                        onTextColor: theme.background
                        onClicked: backend.togglePlay(win.chainStart())
                    }
                    Chip {
                        objectName: "record"
                        display: true
                        implicitHeight: win.headerHeight * 0.6
                        icon: "record"
                        text: win.compact ? "" : qsTr("Rec")
                        ink: theme.colors.red
                        on: backend.recording
                        onColor: theme.colors.red
                        onTextColor: theme.background
                        onClicked: backend.toggleRecord(win.chainStart())
                    }
                }
                Rule { vertical: true }

                // The tempo is the biggest thing on the sheet. The wheel over
                // it turns it, or the swing with Shift.
                RowLayout {
                    id: tempo
                    Layout.leftMargin: 20
                    Layout.rightMargin: 16
                    spacing: 10
                    Label {
                        objectName: "bpm"
                        text: backend.bpm.toLocaleString(Qt.locale(translator.language), 'f', 0)
                        font.family: theme.displayFont
                        Layout.preferredHeight: win.headerHeight
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: win.headerHeight * 0.82
                        topPadding: win.headerHeight * 0.1
                        MouseArea {
                            anchors.fill: parent
                            onWheel: (wheel) => {
                                const up = wheel.angleDelta.y > 0 ? 1 : -1
                                if (wheel.modifiers & Qt.ShiftModifier)
                                    backend.changeSwing(up)
                                else
                                    backend.changeBpm(up)
                            }
                        }
                    }
                    ColumnLayout {
                        spacing: 0
                        Label {
                            text: "BPM"
                            font.pixelSize: 11
                            font.letterSpacing: 0.9
                            color: theme.quiet
                        }
                        Row {
                            spacing: 5
                            Label {
                                objectName: "swing"
                                text: backend.swing.toLocaleString(Qt.locale(translator.language), 'f', 0) + "%"
                                font.bold: true
                            }
                            Label {
                                text: qsTr("swing")
                                font.bold: true
                            }
                        }
                    }
                }
                Rule { vertical: true }

                Item { Layout.fillWidth: true }

                RowLayout {
                    id: effects
                    Layout.rightMargin: 22
                    spacing: 8
                    Chip {
                        objectName: "grit"
                        text: Names.grit(backend.grit)
                        on: backend.grit > 0
                        onClicked: backend.cycleGrit()
                    }
                    Chip {
                        objectName: "vinyl"
                        text: qsTr("Vinyl")
                        on: backend.vinyl
                        onClicked: backend.toggleVinyl()
                    }
                    Chip {
                        objectName: "masterEffects"
                        text: qsTr("Master FX")
                        onClicked: chain.openFor(-1)
                    }
                    Chip {
                        objectName: "mixerButton"
                        text: qsTr("Mixer")
                        onClicked: mixer.open()
                    }
                }
            }
            Rule {}

            RowLayout {
                id: body
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                PadPanel {
                    id: pads
                    objectName: "pads"
                    // A layout in a layout fills by default.
                    Layout.fillWidth: false
                    Layout.preferredWidth: Math.round(Math.max(340, Math.min(540, win.width * 0.36)))
                    Layout.fillHeight: true
                    padHeight: Math.max(52, Math.min((width - 70) / 4 * 0.86, (body.height * 0.66 - 82) / 4))
                    onChopRequested: chop.open()
                    onEffectsRequested: chain.openFor(backend.selectedPad)
                    onLanesRequested: win.showLanes(backend.selectedPad)
                }
                Rule { vertical: true }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 0
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    spacing: 0

                    // The patterns, A to H, as tabs as wide as they are long.
                    RowLayout {
                        id: rail
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        clip: true
                        Layout.preferredHeight: 56
                        spacing: 4
                        readonly property real room: width - railControls.width - 12 - 7 * spacing
                        readonly property var lengths: backend.patternLengths
                        function shown(p) { return backend.usedPatterns[p] || backend.editPattern === p }
                        readonly property int emptyCount: {
                            let n = 0
                            for (let p = 0; p < 8; p++)
                                if (!shown(p))
                                    n++
                            return n
                        }
                        readonly property int usedBars: {
                            let n = 0
                            for (let p = 0; p < 8; p++)
                                if (shown(p))
                                    n += lengths[p] / 16
                            return n
                        }
                        readonly property int emptyWidth: width < 640 ? 34 : 44
                        readonly property real unit: Math.max(40, Math.min(130, (room - emptyCount * emptyWidth) / Math.max(1, usedBars)))

                        Repeater {
                            model: 8
                            delegate: Item {
                                id: tab
                                required property int index
                                readonly property bool chosen: backend.editPattern === index
                                readonly property bool used: rail.shown(index)
                                readonly property int length: rail.lengths[index] || 16
                                readonly property bool playing: backend.playing && backend.playPattern === index
                                objectName: "pattern" + index
                                Layout.alignment: Qt.AlignBottom
                                implicitWidth: used ? Math.max(40, rail.unit * length / 16) : rail.emptyWidth
                                implicitHeight: chosen ? 48 : used ? 40 : 32
                                clip: true

                                Rectangle {
                                    visible: tab.used
                                    anchors.fill: parent
                                    anchors.bottomMargin: -3
                                    color: tab.chosen ? theme.foreground : "transparent"
                                    border.width: 2
                                    border.color: theme.foreground
                                }
                                DashedBox {
                                    visible: !tab.used
                                    anchors.fill: parent
                                    anchors.bottomMargin: -3
                                    color: theme.quiet
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    Label {
                                        Layout.fillWidth: !tab.used
                                        horizontalAlignment: Text.AlignHCenter
                                        text: win.patternName(tab.index)
                                        font.family: theme.displayFont
                                        font.pixelSize: tab.used ? 24 : 18
                                        color: tab.chosen ? theme.background : tab.used ? theme.foreground : theme.quiet
                                    }
                                    Label {
                                        visible: tab.used && tab.width > 96
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignRight
                                        text: qsTr("%Ln bars", "", tab.length / 16)
                                        font.bold: true
                                        font.pixelSize: 13
                                        elide: Text.ElideRight
                                        color: tab.chosen ? theme.background : theme.foreground
                                    }
                                }
                                // The light bar crosses the tab of the pattern playing.
                                Rectangle {
                                    visible: tab.playing
                                    x: (tab.width - width) * backend.playStep / tab.length
                                    width: 4
                                    height: parent.height
                                    color: tab.chosen ? theme.accent : theme.foreground
                                }
                                TapHandler { onTapped: backend.selectPattern(tab.index) }
                            }
                        }

                        Item { Layout.fillWidth: true }

                        // How long the pattern is, and which of its bars is shown.
                        Row {
                            id: railControls
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 6
                            Chip {
                                objectName: "patternLength"
                                text: qsTr("%Ln bars", "", backend.patternLength / 16)
                                onClicked: backend.cyclePatternLength()
                            }
                            Repeater {
                                model: backend.patternLength / 16
                                delegate: Chip {
                                    required property int index
                                    visible: backend.patternLength > 16
                                    objectName: "bar" + index
                                    implicitWidth: 36
                                    text: (index + 1).toLocaleString(Qt.locale(translator.language), 'f', 0)
                                    on: Math.floor(win.cursorStep / 16) === index
                                    onClicked: win.cursorStep = index * 16 + win.cursorStep % 16
                                    // The bar playing, when it isn't the one shown.
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.bottomMargin: 4
                                        width: parent.width - 16
                                        height: 3
                                        visible: backend.playing && backend.playPattern === backend.editPattern
                                                 && Math.floor(backend.playStep / 16) === parent.index && !parent.on
                                        color: theme.foreground
                                    }
                                }
                            }
                        }
                    }
                    Rule {}

                    PatternGrid {
                        id: grid
                        objectName: "grid"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        cursorStep: win.cursorStep
                        cursorLane: win.cursorLane
                        expanded: win.expanded
                        active: !win.songFocus
                        onCellPicked: (pad, step) => {
                            win.songFocus = false
                            win.cursorStep = step
                        }
                        onLanePicked: (pad, lane, step) => {
                            win.songFocus = false
                            win.cursorLane = lane
                            win.cursorStep = step
                        }
                        onExpandToggled: (pad) => win.toggleExpanded(pad)
                        onAddLaneRequested: (pad) => lanePicker.openFor(pad)
                    }
                }
            }
            Rule {}

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 66
                Layout.leftMargin: 22
                Layout.rightMargin: 22
                spacing: 18

                SongStrip {
                    id: song
                    objectName: "song"
                    Layout.preferredWidth: Math.min(implicitWidth, (win.width - 44) * 0.6)
                    active: win.songFocus
                    cursor: win.songCursor
                    onSlotPicked: (index) => {
                        win.songFocus = true
                        win.songCursor = index
                    }
                }

                // What just happened, or went wrong.
                Label {
                    id: status
                    objectName: "status"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    readonly property string kind: backend.message.kind || ""
                    readonly property bool error: ["saveFailed", "openFailed", "sampleFailed", "noFfmpeg", "exportFailed",
                                                   "noDevice", "deviceFailed", "pickerFailed", "pluginFailed", "editorFailed",
                                                   "scanFailed", "chainFull"].indexOf(kind) >= 0
                    text: {
                        const m = backend.message
                        switch (kind) {
                        case "saved": return qsTr("Saved %1").arg(m.arg)
                        case "exported": return qsTr("Exported %1").arg(m.arg)
                        case "copied": return qsTr("Copied pattern %1").arg(m.arg)
                        case "pasted": return qsTr("Pasted into pattern %1").arg(m.arg)
                        case "chopped": return qsTr("Chopped onto %1 pads from pad %2").arg(m.arg).arg(m.detail)
                        case "saveFailed": return qsTr("Couldn't save %1: %2").arg(m.arg).arg(m.detail)
                        case "openFailed": return qsTr("Couldn't open %1: %2").arg(m.arg).arg(m.detail)
                        case "exportFailed": return qsTr("Couldn't export %1: %2").arg(m.arg).arg(m.detail)
                        case "sampleFailed": return qsTr("Couldn't load the sample %1: %2").arg(m.arg).arg(m.detail)
                        case "noFfmpeg": return qsTr("Loading samples needs ffmpeg, which isn't installed.")
                        case "noDevice": return qsTr("There is no sound output to play through.")
                        case "deviceFailed": return qsTr("The sound output stopped working.")
                        case "pickerFailed": return qsTr("The file chooser didn't open: %1").arg(m.detail)
                        case "pluginFailed": return qsTr("Couldn't load %1: %2").arg(m.arg).arg(m.detail)
                        case "editorFailed": return qsTr("%1 couldn't open its window: %2").arg(m.arg).arg(m.detail)
                        case "scanFailed": return qsTr("Looking for plugins failed: %1").arg(m.detail)
                        case "chainFull": return qsTr("A chain holds %1 effects at most.").arg(m.arg)
                        }
                        return backend.recording ? qsTr("Recording: play the pads, they land on the nearest step")
                                                 : qsTr("Play the pads with 1–4, Q–R, A–F, Z–V. Press ? for the shortcuts")
                    }
                    color: error || backend.recording ? theme.colors.red : theme.quiet
                    elide: Text.ElideRight
                }
            }
        }
    }

    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 26
        background: Rectangle {
            color: theme.background
            border.width: 3
            border.color: theme.foreground
            radius: theme.radius
        }

        GridLayout {
            columns: 4
            columnSpacing: 18
            rowSpacing: 6
            // A popup is not an Item and doesn't inherit the window's
            // mirroring, so its content opts in.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: [
                    { keys: "1–4  Q–R  A–F  Z–V", action: qsTr("Play the pads; with Shift, harder") },
                    { keys: "Space", action: qsTr("Play or stop") },
                    { keys: "Shift+Space", action: qsTr("Record the pads into the pattern") },
                    { keys: "N  Shift+N", action: qsTr("Note repeat on or off, and its timing") },
                    { keys: "P", action: qsTr("Play the pattern or the song") },
                    { keys: "[ ]  { }", action: qsTr("Tempo down or up, by 1 or 10") },
                    { keys: ", .", action: qsTr("Less or more swing") },
                    { keys: "G  Shift+G", action: qsTr("Grit, and vinyl") },
                    { keys: "← → ↑ ↓", action: qsTr("Move between steps and pads") },
                    { keys: "Enter", action: qsTr("Turn a step on or off") },
                    { keys: "Delete", action: qsTr("Clear a step") },
                    { keys: "Shift+↑ ↓", action: qsTr("A step louder or softer, or a lane up or down") },
                    { keys: "- =  _ +", action: qsTr("Tune the pad by a semitone or an octave") },
                    { keys: "Shift+← →", action: qsTr("Pad volume") },
                    { keys: "M  Shift+M", action: qsTr("Mute or solo the pad") },
                    { keys: "U", action: qsTr("The pad's choke group") },
                    { keys: "O  Shift+O", action: qsTr("Load a sample onto the pad, or go back to the kit") },
                    { keys: "K", action: qsTr("Chop a sample onto the pads") },
                    { keys: "J  Shift+J", action: qsTr("The pad's synth and effects, or the master's") },
                    { keys: "B  Shift+B", action: qsTr("The next or the previous bank of pads") },
                    { keys: "L  Shift+L", action: qsTr("Open the track to its lanes, or add one") },
                    { keys: "T  Y", action: qsTr("A step's roll, and its chance") },
                    { keys: "Alt+← →", action: qsTr("Nudge a step early or late") },
                    { keys: "Ctrl+← →", action: qsTr("The bar before or after") },
                    { keys: "Ctrl+B", action: qsTr("Make the pattern one to four bars") },
                    { keys: "Ctrl+M", action: qsTr("The mixer") },
                    { keys: "Ctrl+1 – 8  PgUp PgDn", action: qsTr("Edit pattern A to H") },
                    { keys: "Ctrl+C  Ctrl+V", action: qsTr("Copy and paste a pattern") },
                    { keys: "Ctrl+Delete", action: qsTr("Clear the pattern") },
                    { keys: "Tab", action: qsTr("Switch between the steps and the song") },
                    { keys: "Ctrl+1 – 8", action: qsTr("In the song: set or add a pattern") },
                    { keys: "I  Delete", action: qsTr("In the song: insert or remove a slot") },
                    { keys: "Ctrl+Z  Ctrl+Shift+Z", action: qsTr("Undo or redo") },
                    { keys: "Ctrl+N  Ctrl+O", action: qsTr("New or open a song") },
                    { keys: "Ctrl+S  Ctrl+Shift+S", action: qsTr("Save, or save as") },
                    { keys: "Ctrl+E", action: qsTr("Export the song as WAV") },
                    { keys: "?", action: qsTr("Show or hide the shortcuts") },
                    { keys: "Ctrl+Q", action: qsTr("Quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.columnSpan: 2
                    spacing: 14
                    Label {
                        Layout.preferredWidth: 196
                        text: modelData.keys
                        color: theme.accent
                        font.bold: true
                    }
                    Label {
                        Layout.preferredWidth: 280
                        text: modelData.action
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    Mixer {
        id: mixer
        objectName: "mixer"
        rightToLeft: win.rightToLeft
        onEffectsRequested: (pad) => {
            mixer.close()
            chain.openFor(pad)
        }
    }

    LanePicker {
        id: lanePicker
        objectName: "lanePicker"
        rightToLeft: win.rightToLeft
        onPicked: (id) => {
            backend.addLane(pad, id)
            win.expand(pad)
            backend.selectPad(pad)
            // Onto the new lane.
            const lanes = win.bankRow(pad).lanes
            for (let i = 0; i < lanes.length; i++)
                if (lanes[i].id === id)
                    win.cursorLane = i
        }
    }

    ChainView {
        id: chain
        objectName: "chain"
        rightToLeft: win.rightToLeft
    }

    ChopView {
        id: chop
        objectName: "chop"
        rightToLeft: win.rightToLeft
    }

    ConfirmDialog {
        id: confirm
        objectName: "confirm"
        rightToLeft: win.rightToLeft
        text: backend.fileName ? qsTr("Save the changes to %1 first?").arg(backend.fileName)
                               : qsTr("Save this song first?")
        onSaveChosen: {
            waiting = true
            backend.save()
        }
        onDiscardChosen: {
            const action = confirm.action
            confirm.action = null
            action()
        }
    }
}
