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

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    // Keys go to the window only while no dialog is up.
    readonly property bool idle: !help.opened && !confirm.opened && !chop.opened
    property int cursorStep: 0
    property bool songFocus: false
    property int songCursor: 0
    property bool closing: false

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function patternName(p) { return String.fromCharCode(65 + p) }
    function chainStart() { return Math.min(songCursor, Math.max(0, backend.chain.length - 1)) }

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

    // Patterns.
    Shortcut { sequence: "Ctrl+C"; enabled: win.idle; onActivated: backend.copyPattern() }
    Shortcut { sequence: "Ctrl+V"; enabled: win.idle; onActivated: backend.pastePattern() }
    Shortcut { sequence: "Ctrl+Delete"; enabled: win.idle; onActivated: backend.clearPattern() }
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
    Shortcut { sequence: "?"; enabled: !confirm.opened && !chop.opened; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequences: ["Ctrl+Q"]; enabled: win.idle; onActivated: win.close() }
    Shortcut {
        sequence: "Escape"
        enabled: win.idle
        onActivated: win.songFocus ? win.songFocus = false : backend.dismissMessage()
    }

    // The steps.
    Shortcut { sequence: "Left"; enabled: win.idle && !win.songFocus; onActivated: win.cursorStep = (win.cursorStep + 15) % 16 }
    Shortcut { sequence: "Right"; enabled: win.idle && !win.songFocus; onActivated: win.cursorStep = (win.cursorStep + 1) % 16 }
    Shortcut { sequence: "Up"; enabled: win.idle && !win.songFocus; onActivated: backend.selectPad((backend.selectedPad + 15) % 16) }
    Shortcut { sequence: "Down"; enabled: win.idle && !win.songFocus; onActivated: backend.selectPad((backend.selectedPad + 1) % 16) }
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: win.idle && !win.songFocus
        onActivated: backend.toggleStep(backend.selectedPad, win.cursorStep)
    }
    Shortcut {
        sequences: ["Delete", "Backspace"]
        enabled: win.idle && !win.songFocus
        onActivated: backend.clearStep(backend.selectedPad, win.cursorStep)
    }
    Shortcut { sequence: "Shift+Up"; enabled: win.idle && !win.songFocus; onActivated: backend.changeStep(backend.selectedPad, win.cursorStep, 1) }
    Shortcut { sequence: "Shift+Down"; enabled: win.idle && !win.songFocus; onActivated: backend.changeStep(backend.selectedPad, win.cursorStep, -1) }

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
                backend.padDown(pad, event.modifiers & Qt.ShiftModifier ? 4 : 3)
        }
        Keys.onReleased: (event) => {
            const pad = win.padFor(event)
            if (pad < 0)
                return
            event.accepted = true
            if (!event.isAutoRepeat)
                backend.padUp(pad)
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 12

            // Transport: play, record, tempo, swing, and the sound's grit.
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Chip {
                    objectName: "play"
                    text: backend.playing ? "■  " + qsTr("Stop") : "▶  " + qsTr("Play")
                    on: backend.playing
                    implicitHeight: 36
                    onClicked: backend.togglePlay(win.chainStart())
                }
                Chip {
                    objectName: "record"
                    text: "●  " + qsTr("Rec")
                    on: backend.recording
                    onColor: theme.colors.red
                    onTextColor: theme.background
                    implicitHeight: 36
                    onClicked: backend.toggleRecord(win.chainStart())
                }
                Chip {
                    objectName: "repeat"
                    text: qsTr("Repeat %1").arg(backend.repeatName)
                    on: backend.repeat > 0
                    bold: false
                    implicitHeight: 36
                    onClicked: backend.toggleRepeat()
                    onPressAndHold: backend.cycleRepeat()
                }

                Item { implicitWidth: 6 }

                Row {
                    spacing: 5
                    Label {
                        objectName: "bpm"
                        text: backend.bpm.toLocaleString(Qt.locale(translator.language), 'f', 0)
                        font.pixelSize: 24
                        font.bold: true
                        anchors.baseline: bpmUnit.baseline
                    }
                    Label {
                        id: bpmUnit
                        text: "BPM"
                        color: theme.colors.muted
                        topPadding: 9
                    }
                }
                Row {
                    spacing: 5
                    Label {
                        objectName: "swing"
                        text: backend.swing.toLocaleString(Qt.locale(translator.language), 'f', 0) + "%"
                        font.pixelSize: 24
                        font.bold: true
                        anchors.baseline: swingUnit.baseline
                    }
                    Label {
                        id: swingUnit
                        text: qsTr("swing")
                        color: theme.colors.muted
                        topPadding: 9
                    }
                }

                Item { implicitWidth: 6 }

                Chip {
                    objectName: "grit"
                    text: Names.grit(backend.grit)
                    on: backend.grit > 0
                    bold: false
                    onClicked: backend.cycleGrit()
                }
                Chip {
                    objectName: "vinyl"
                    text: qsTr("Vinyl")
                    on: backend.vinyl
                    bold: false
                    onClicked: backend.toggleVinyl()
                }

                // What Space plays: the pattern being edited, or the song.
                Row {
                    objectName: "mode"
                    Repeater {
                        model: [qsTr("Pattern"), qsTr("Song")]
                        delegate: Rectangle {
                            required property int index
                            required property string modelData
                            readonly property bool chosen: (index === 1) === backend.songMode
                            implicitWidth: modeLabel.implicitWidth + 22
                            implicitHeight: 32
                            radius: Math.min(4, theme.radius)
                            color: chosen ? theme.colors.selection : "transparent"
                            border.color: theme.colors.selection
                            Label {
                                id: modeLabel
                                anchors.centerIn: parent
                                text: parent.modelData
                                color: parent.chosen ? theme.foreground : theme.colors.muted
                            }
                            TapHandler { onTapped: if (!parent.chosen) backend.toggleSongMode() }
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    objectName: "exporting"
                    visible: backend.exporting
                    text: qsTr("Exporting…")
                    color: theme.colors.muted
                }
                Label {
                    objectName: "fileName"
                    Layout.maximumWidth: 220
                    text: (backend.fileName || qsTr("Untitled")) + (backend.dirty ? " •" : "")
                    elide: Text.ElideMiddle
                    color: theme.colors.muted
                }
            }

            // The patterns, A to H.
            RowLayout {
                spacing: 6

                Label {
                    text: qsTr("Pattern")
                    font.bold: true
                    color: win.songFocus ? theme.foreground : theme.accent
                    Layout.rightMargin: 6
                }

                Repeater {
                    model: 8
                    delegate: Rectangle {
                        required property int index
                        readonly property bool chosen: backend.editPattern === index
                        readonly property bool used: backend.usedPatterns[index]
                        objectName: "pattern" + index
                        implicitWidth: 36
                        implicitHeight: 32
                        radius: Math.min(4, theme.radius)
                        color: chosen ? theme.accent : used ? win.alpha(theme.colors.selection, 0.9) : "transparent"
                        border.color: chosen ? theme.accent : theme.colors.selection
                        Label {
                            anchors.centerIn: parent
                            text: win.patternName(parent.index)
                            font.bold: true
                            color: parent.chosen ? theme.accentForeground : parent.used ? theme.foreground : theme.colors.muted
                        }
                        // The pattern playing, when it isn't the one shown.
                        Rectangle {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 3
                            width: 6
                            height: 6
                            radius: 3
                            visible: backend.playing && backend.playPattern === parent.index && !parent.chosen
                            color: theme.accent
                        }
                        TapHandler { onTapped: backend.selectPattern(parent.index) }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 20

                PatternGrid {
                    id: grid
                    objectName: "grid"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    cursorStep: win.cursorStep
                    active: !win.songFocus
                    onCellPicked: (pad, step) => {
                        win.songFocus = false
                        win.cursorStep = step
                    }
                }

                PadPanel {
                    id: pads
                    objectName: "pads"
                    // A layout in a layout fills by default.
                    Layout.fillWidth: false
                    Layout.preferredWidth: Math.min(380, win.width * 0.3)
                    Layout.maximumWidth: Layout.preferredWidth
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignTop
                    onChopRequested: chop.open()
                }
            }

            SongStrip {
                id: song
                objectName: "song"
                Layout.fillWidth: true
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
                readonly property string kind: backend.message.kind || ""
                readonly property bool error: ["saveFailed", "openFailed", "sampleFailed", "noFfmpeg", "exportFailed",
                                               "noDevice", "deviceFailed", "pickerFailed"].indexOf(kind) >= 0
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
                    }
                    return backend.recording ? qsTr("Recording: play the pads, they land on the nearest step")
                                             : qsTr("Play the pads with 1–4, Q–R, A–F, Z–V. Press ? for the shortcuts")
                }
                color: error || backend.recording ? theme.colors.red : theme.colors.muted
                elide: Text.ElideRight
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
            border.color: theme.colors.selection
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
                    { keys: "Shift+↑ ↓", action: qsTr("A step louder or softer") },
                    { keys: "- =  _ +", action: qsTr("Tune the pad by a semitone or an octave") },
                    { keys: "Shift+← →", action: qsTr("Pad volume") },
                    { keys: "M  Shift+M", action: qsTr("Mute or solo the pad") },
                    { keys: "U", action: qsTr("The pad's mute group") },
                    { keys: "O  Shift+O", action: qsTr("Load a sample onto the pad, or go back to the kit") },
                    { keys: "K", action: qsTr("Chop a sample onto the pads") },
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
                        Layout.preferredWidth: 160
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
