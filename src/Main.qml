import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1120
    height: 720
    minimumWidth: 880
    minimumHeight: 560
    visible: true
    title: (backend.fileName || qsTr("Untitled")) + (backend.dirty ? " •" : "") + " — Dole"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    // Keys go to the window only while no dialog is up.
    readonly property bool idle: !help.opened && !confirm.opened
    property int cursorTrack: 0
    property int cursorStep: 0
    property bool songFocus: false
    property int songCursor: 0
    property bool closing: false

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function patternName(p) { return String.fromCharCode(65 + p) }

    function moveCursor(tracks, steps) {
        cursorTrack = (cursorTrack + tracks + 8) % 8
        cursorStep = (cursorStep + steps + 16) % 16
    }

    function moveSongCursor(delta) {
        songCursor = Math.max(0, Math.min(backend.chain.length, songCursor + delta))
    }

    // Typing a pattern into the song sets the slot under the cursor, or adds
    // one at the end, and moves on, so "1 1 2 1" writes A A B A.
    function typePattern(p) {
        if (songCursor >= backend.chain.length)
            backend.chainInsert(songCursor, p)
        else
            backend.chainSet(songCursor, p)
        songCursor = songCursor + 1
    }

    function play() {
        backend.togglePlay(Math.min(songCursor, Math.max(0, backend.chain.length - 1)))
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

    // Anywhere.
    Shortcut { sequence: "Space"; enabled: win.idle; onActivated: win.play() }
    Shortcut { sequence: "Tab"; enabled: win.idle; onActivated: win.songFocus = !win.songFocus }
    Shortcut { sequence: "P"; enabled: win.idle; onActivated: backend.toggleSongMode() }
    Shortcut { sequence: "]"; enabled: win.idle; onActivated: backend.changeBpm(1) }
    Shortcut { sequence: "["; enabled: win.idle; onActivated: backend.changeBpm(-1) }
    Shortcut { sequence: "}"; enabled: win.idle; onActivated: backend.changeBpm(10) }
    Shortcut { sequence: "{"; enabled: win.idle; onActivated: backend.changeBpm(-10) }
    Shortcut { sequence: "."; enabled: win.idle; onActivated: backend.changeSwing(1) }
    Shortcut { sequence: ","; enabled: win.idle; onActivated: backend.changeSwing(-1) }
    Shortcut { sequence: "M"; enabled: win.idle; onActivated: backend.toggleMute(win.cursorTrack) }
    Shortcut { sequence: "S"; enabled: win.idle; onActivated: backend.toggleSolo(win.cursorTrack) }
    Shortcut { sequence: "O"; enabled: win.idle; onActivated: backend.chooseSample(win.cursorTrack) }
    Shortcut { sequence: "Shift+O"; enabled: win.idle; onActivated: backend.resetSample(win.cursorTrack) }
    Shortcut { sequence: "Shift+Right"; enabled: win.idle; onActivated: backend.changeVolume(win.cursorTrack, 0.05) }
    Shortcut { sequence: "Shift+Left"; enabled: win.idle; onActivated: backend.changeVolume(win.cursorTrack, -0.05) }
    Shortcut { sequence: "Ctrl+C"; enabled: win.idle; onActivated: backend.copyPattern() }
    Shortcut { sequence: "Ctrl+V"; enabled: win.idle; onActivated: backend.pastePattern() }
    Shortcut { sequence: "Ctrl+Delete"; enabled: win.idle; onActivated: backend.clearPattern() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: win.idle; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; enabled: win.idle; onActivated: backend.redo() }
    Shortcut { sequences: [StandardKey.New]; enabled: win.idle; onActivated: win.guard(backend.newSong) }
    Shortcut { sequences: [StandardKey.Open]; enabled: win.idle; onActivated: win.guard(backend.openSong) }
    Shortcut { sequences: [StandardKey.Save]; enabled: win.idle; onActivated: backend.save() }
    Shortcut { sequences: ["Ctrl+Shift+S"]; enabled: win.idle; onActivated: backend.saveAs() }
    Shortcut { sequence: "Ctrl+E"; enabled: win.idle; onActivated: backend.exportAudio() }
    Shortcut { sequence: "?"; enabled: !confirm.opened; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequence: "Q"; enabled: win.idle; onActivated: win.close() }
    Shortcut {
        sequence: "Escape"
        enabled: win.idle
        onActivated: win.songFocus ? win.songFocus = false : backend.dismissMessage()
    }

    // The steps.
    Shortcut { sequences: ["Left", "H"]; enabled: win.idle && !win.songFocus; onActivated: win.moveCursor(0, -1) }
    Shortcut { sequences: ["Right", "L"]; enabled: win.idle && !win.songFocus; onActivated: win.moveCursor(0, 1) }
    Shortcut { sequences: ["Up", "K"]; enabled: win.idle && !win.songFocus; onActivated: win.moveCursor(-1, 0) }
    Shortcut { sequences: ["Down", "J"]; enabled: win.idle && !win.songFocus; onActivated: win.moveCursor(1, 0) }
    Shortcut {
        sequences: ["Return", "Enter", "X"]
        enabled: win.idle && !win.songFocus
        onActivated: backend.toggleStep(win.cursorTrack, win.cursorStep)
    }
    Shortcut {
        sequences: ["Shift+Return", "Shift+Enter"]
        enabled: win.idle && !win.songFocus
        onActivated: backend.holdStep(win.cursorTrack, win.cursorStep)
    }
    Shortcut {
        sequences: ["Delete", "Backspace"]
        enabled: win.idle && !win.songFocus
        onActivated: backend.clearStep(win.cursorTrack, win.cursorStep)
    }
    Shortcut { sequence: "Shift+Up"; enabled: win.idle && !win.songFocus; onActivated: backend.changeStep(win.cursorTrack, win.cursorStep, 1) }
    Shortcut { sequence: "Shift+Down"; enabled: win.idle && !win.songFocus; onActivated: backend.changeStep(win.cursorTrack, win.cursorStep, -1) }
    Shortcut { sequence: "Ctrl+Shift+Up"; enabled: win.idle && !win.songFocus; onActivated: backend.changeStep(win.cursorTrack, win.cursorStep, 12) }
    Shortcut { sequence: "Ctrl+Shift+Down"; enabled: win.idle && !win.songFocus; onActivated: backend.changeStep(win.cursorTrack, win.cursorStep, -12) }

    // The song.
    Shortcut { sequences: ["Left", "H"]; enabled: win.idle && win.songFocus; onActivated: win.moveSongCursor(-1) }
    Shortcut { sequences: ["Right", "L"]; enabled: win.idle && win.songFocus; onActivated: win.moveSongCursor(1) }
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

    // Patterns 1 to 8: the one to edit, or, in the song, the one to play.
    Repeater {
        model: 8
        Item {
            required property int index
            Shortcut {
                sequence: String(index + 1)
                enabled: win.idle
                onActivated: win.songFocus ? win.typePattern(index) : backend.selectPattern(index)
            }
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 16

            // Transport: playing, tempo, swing, and what plays.
            RowLayout {
                Layout.fillWidth: true
                spacing: 24

                Rectangle {
                    objectName: "transport"
                    implicitWidth: playLabel.implicitWidth + 28
                    implicitHeight: 34
                    radius: Math.min(17, theme.radius)
                    color: backend.playing ? theme.accent : win.alpha(theme.colors.selection, 0.8)
                    Label {
                        id: playLabel
                        anchors.centerIn: parent
                        text: backend.playing ? "■  " + qsTr("Stop") : "▶  " + qsTr("Play")
                        color: backend.playing ? theme.accentForeground : theme.foreground
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: win.play()
                    }
                }

                Row {
                    spacing: 6
                    Label {
                        objectName: "bpm"
                        text: backend.bpm.toLocaleString(Qt.locale(translator.language), 'f', 0)
                        font.pixelSize: 26
                        font.bold: true
                        anchors.baseline: bpmUnit.baseline
                    }
                    Label {
                        id: bpmUnit
                        text: "BPM"
                        color: theme.colors.muted
                        topPadding: 10
                    }
                }

                Row {
                    spacing: 6
                    Label {
                        objectName: "swing"
                        text: backend.swing.toLocaleString(Qt.locale(translator.language), 'f', 0) + "%"
                        font.pixelSize: 26
                        font.bold: true
                        anchors.baseline: swingUnit.baseline
                    }
                    Label {
                        id: swingUnit
                        text: qsTr("swing")
                        color: theme.colors.muted
                        topPadding: 10
                    }
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
                            implicitWidth: modeLabel.implicitWidth + 24
                            implicitHeight: 30
                            radius: Math.min(4, theme.radius)
                            color: chosen ? win.alpha(theme.colors.selection, 1) : "transparent"
                            border.color: theme.colors.selection
                            Label {
                                id: modeLabel
                                anchors.centerIn: parent
                                text: parent.modelData
                                color: parent.chosen ? theme.foreground : theme.colors.muted
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: if (!parent.chosen) backend.toggleSongMode()
                            }
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
                    Layout.maximumWidth: 260
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
                        implicitWidth: 34
                        implicitHeight: 30
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
                        MouseArea {
                            anchors.fill: parent
                            onClicked: backend.selectPattern(parent.index)
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            PatternGrid {
                id: grid
                objectName: "grid"
                Layout.fillWidth: true
                Layout.fillHeight: true
                cursorTrack: win.cursorTrack
                cursorStep: win.cursorStep
                active: !win.songFocus
                onCellPicked: (track, step) => {
                    win.songFocus = false
                    win.cursorTrack = track
                    win.cursorStep = step
                }
                onTrackPicked: (track) => {
                    win.songFocus = false
                    win.cursorTrack = track
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
                    case "saveFailed": return qsTr("Couldn't save %1: %2").arg(m.arg).arg(m.detail)
                    case "openFailed": return qsTr("Couldn't open %1: %2").arg(m.arg).arg(m.detail)
                    case "exportFailed": return qsTr("Couldn't export %1: %2").arg(m.arg).arg(m.detail)
                    case "sampleFailed": return qsTr("Couldn't load the sample %1: %2").arg(m.arg).arg(m.detail)
                    case "noFfmpeg": return qsTr("Loading samples needs ffmpeg, which isn't installed.")
                    case "noDevice": return qsTr("There is no sound output to play through.")
                    case "deviceFailed": return qsTr("The sound output stopped working.")
                    case "pickerFailed": return qsTr("The file chooser didn't open: %1").arg(m.detail)
                    }
                    return qsTr("Press ? for the shortcuts")
                }
                color: error ? theme.colors.red : theme.colors.muted
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
        padding: 28
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        GridLayout {
            columns: 4
            columnSpacing: 18
            rowSpacing: 7
            // A popup is not an Item and doesn't inherit the window's
            // mirroring, so its content opts in.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: [
                    { keys: "Space", action: qsTr("Play or stop") },
                    { keys: "P", action: qsTr("Play the pattern or the song") },
                    { keys: "← → ↑ ↓  h j k l", action: qsTr("Move between steps") },
                    { keys: "Enter  x", action: qsTr("Turn a step on or off") },
                    { keys: "Shift+Enter", action: qsTr("Hold the bass note through a step") },
                    { keys: "Delete", action: qsTr("Clear a step") },
                    { keys: "Shift+↑ ↓", action: qsTr("Louder or softer; a bass note up or down") },
                    { keys: "Ctrl+Shift+↑ ↓", action: qsTr("A bass note up or down an octave") },
                    { keys: "1 – 8", action: qsTr("Edit pattern A to H") },
                    { keys: "Ctrl+C  Ctrl+V", action: qsTr("Copy and paste a pattern") },
                    { keys: "Ctrl+Delete", action: qsTr("Clear the pattern") },
                    { keys: "[ ]  { }", action: qsTr("Tempo down or up, by 1 or 10") },
                    { keys: ", .", action: qsTr("Less or more swing") },
                    { keys: "M  S", action: qsTr("Mute or solo the track") },
                    { keys: "Shift+← →", action: qsTr("Track volume") },
                    { keys: "O", action: qsTr("Load a sample onto the track") },
                    { keys: "Shift+O", action: qsTr("Back to the built-in sound") },
                    { keys: "Tab", action: qsTr("Switch between the steps and the song") },
                    { keys: "1 – 8", action: qsTr("In the song: set or add a pattern") },
                    { keys: "I  Delete", action: qsTr("In the song: insert or remove a slot") },
                    { keys: "Enter", action: qsTr("In the song: edit the pattern") },
                    { keys: "Ctrl+Z  Ctrl+Shift+Z", action: qsTr("Undo or redo") },
                    { keys: "Ctrl+N  Ctrl+O", action: qsTr("New or open a song") },
                    { keys: "Ctrl+S  Ctrl+Shift+S", action: qsTr("Save, or save as") },
                    { keys: "Ctrl+E", action: qsTr("Export the song as WAV") },
                    { keys: "?", action: qsTr("Show or hide the shortcuts") },
                    { keys: "Q", action: qsTr("Quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.columnSpan: 2
                    spacing: 14
                    Label {
                        Layout.preferredWidth: 150
                        text: modelData.keys
                        color: theme.accent
                        font.bold: true
                    }
                    Label {
                        Layout.preferredWidth: 270
                        text: modelData.action
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
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
