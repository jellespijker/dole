import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Names.js" as Names

// The sixteen pads of a bank, four by four with pad 1 at the bottom left as
// on an MPC, and the settings of the pad last hit. Each pad answers its own
// finger, so several can be played at once on a touch screen.
ColumnLayout {
    id: panel

    signal chopRequested()
    signal effectsRequested()
    signal lanesRequested()

    spacing: 10

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function number(n, digits) { return n.toLocaleString(Qt.locale(translator.language), 'f', digits || 0) }
    // How the filter and the envelope read, the way the renderer maps them.
    function hertz(f) { return f >= 1000 ? qsTr("%1 kHz").arg(number(f / 1000, 1)) : qsTr("%1 Hz").arg(number(f)) }
    function filterText(f) {
        if (Math.abs(f) < 0.005)
            return qsTr("Open")
        return f < 0 ? qsTr("Low %1").arg(hertz(20000 * Math.pow(60 / 20000, -f)))
                     : qsTr("High %1").arg(hertz(20 * Math.pow(400, f)))
    }
    function seconds(s) { return s < 1 ? qsTr("%1 ms").arg(number(s * 1000)) : qsTr("%1 s").arg(number(s, 1)) }
    function panText(p) {
        if (Math.abs(p) < 0.005)
            return qsTr("Middle")
        return p < 0 ? qsTr("Left %1").arg(number(-p * 100)) : qsTr("Right %1").arg(number(p * 100))
    }

    readonly property int pad: backend.selectedPad
    readonly property var selected: backend.pads[pad]

    // Banks A to D.
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Label {
            text: qsTr("Bank")
            color: theme.colors.muted
        }
        Repeater {
            model: 4
            delegate: Chip {
                required property int index
                objectName: "bank" + index
                implicitHeight: 28
                Layout.fillWidth: true
                text: String.fromCharCode(65 + index)
                on: backend.bank === index
                onClicked: backend.setBank(index)
            }
        }
    }

    GridLayout {
        id: pads
        Layout.fillWidth: true
        // The pads leave the settings room on a short window.
        // A layout in a layout fills by default; the settings take the rest.
        Layout.fillHeight: false
        Layout.preferredHeight: Math.min(panel.width, Window.height * 0.36)
        columns: 4
        rowSpacing: 6
        columnSpacing: 6
        // Pads sit where an MPC has them in every language.
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true

        Repeater {
            model: 16

            delegate: Rectangle {
                id: pad
                required property int index
                // Top row first: 13 to 16, down to 1 to 4.
                readonly property int inBank: (3 - Math.floor(index / 4)) * 4 + index % 4
                readonly property int number: backend.bank * 16 + inBank
                readonly property var info: backend.pads[number]
                readonly property bool chosen: backend.selectedPad === number
                property real glow: 0

                objectName: "pad" + number
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Math.min(8, theme.radius)
                color: Qt.tint(panel.alpha(theme.colors.selection, info.state === "empty" ? 0.4 : 0.85),
                               panel.alpha(theme.accent, glow * 0.85))
                border.width: chosen ? 2 : 1
                border.color: chosen ? theme.accent : theme.colors.selection
                opacity: info.audible ? 1 : 0.45

                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 6
                    text: pad.info.name
                    font.pixelSize: 11
                    font.bold: true
                    color: pad.chosen ? theme.accent : theme.colors.muted
                }
                Label {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    text: Names.keys[pad.inBank]
                    font.pixelSize: 10
                    color: theme.colors.muted
                }
                // The pad runs through effects, or plays a synth.
                Label {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 6
                    visible: pad.info.effects > 0 || pad.info.synth !== ""
                    text: (pad.info.synth !== "" ? "♪" : "") + (pad.info.effects > 0 ? "FX" : "")
                    font.pixelSize: 9
                    font.bold: true
                    color: theme.accent
                }
                Label {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 6
                    anchors.rightMargin: 22
                    text: Names.pad(pad.info)
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    font.bold: true
                    color: pad.info.state === "failed" ? theme.colors.red
                         : pad.info.state === "empty" ? theme.colors.muted : theme.foreground
                }

                NumberAnimation on glow {
                    id: flash
                    running: false
                    from: 1
                    to: 0
                    duration: 220
                    easing.type: Easing.OutQuad
                }
                Connections {
                    target: backend
                    function onPadHit(hit) {
                        if (hit === pad.number)
                            flash.restart()
                    }
                }

                // Sounds on the press, not the release, and repeats while held.
                TapHandler {
                    gesturePolicy: TapHandler.WithinBounds
                    onPressedChanged: pressed ? backend.padDown(pad.number) : backend.padUp(pad.number)
                }
            }
        }
    }

    // The pad last hit.
    RowLayout {
        Layout.fillWidth: true
        Label {
            objectName: "padTitle"
            Layout.fillWidth: true
            text: qsTr("Pad %1").arg(panel.selected.name) + "  " + Names.pad(panel.selected)
            font.bold: true
            font.pixelSize: 15
            elide: Text.ElideRight
        }
        Label {
            text: panel.selected.synth !== "" ? qsTr("Synth")
                  : panel.selected.state === "kit" ? qsTr("Kit")
                  : panel.selected.state === "empty" ? qsTr("Empty")
                  : panel.selected.state === "loading" ? qsTr("Loading…")
                  : panel.selected.state === "failed" ? qsTr("Can't load")
                  : panel.selected.chop ? qsTr("Chop") : qsTr("Sample")
            color: panel.selected.state === "failed" ? theme.colors.red : theme.colors.muted
            font.pixelSize: 12
        }
    }

    // Its sound, its place, and its timing, scrolled on a short window.
    Flickable {
        id: settings
        objectName: "padSettings"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentHeight: settingsColumn.height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: settingsColumn
            width: settings.width - 10
            spacing: 6

            RowLayout {
                width: parent.width
                spacing: 6
                Label { Layout.preferredWidth: 78; text: qsTr("Tune"); color: theme.colors.muted }
                Chip { implicitHeight: 28; text: "−"; onClicked: backend.changeTune(panel.pad, -1) }
                Label {
                    objectName: "tune"
                    Layout.minimumWidth: 36
                    horizontalAlignment: Text.AlignHCenter
                    text: (panel.selected.tune > 0 ? "+" : "") + panel.number(panel.selected.tune)
                    font.bold: true
                }
                Chip { implicitHeight: 28; text: "+"; onClicked: backend.changeTune(panel.pad, 1) }
                Item { Layout.fillWidth: true }
            }

            Repeater {
                model: [
                    { name: "volume", label: qsTr("Volume"), from: 0, to: 1 },
                    { name: "pan", label: qsTr("Pan"), from: -1, to: 1 },
                    { name: "filter", label: qsTr("Filter"), from: -1, to: 1 },
                    { name: "resonance", label: qsTr("Resonance"), from: 0, to: 1 },
                    { name: "attack", label: qsTr("Attack"), from: 0, to: 1 },
                    { name: "decay", label: qsTr("Decay"), from: 0, to: 1 }
                ]
                delegate: RowLayout {
                    id: setting
                    required property var modelData
                    readonly property real value: panel.selected[modelData.name]
                    width: settingsColumn.width
                    spacing: 6
                    Label {
                        Layout.preferredWidth: 78
                        text: setting.modelData.label
                        color: theme.colors.muted
                        elide: Text.ElideRight
                    }
                    Slider {
                        objectName: "setting_" + setting.modelData.name
                        Layout.fillWidth: true
                        implicitHeight: 30
                        from: setting.modelData.from
                        to: setting.modelData.to
                        value: setting.value
                        focusPolicy: Qt.NoFocus
                        // Left is left in every language.
                        LayoutMirroring.enabled: false
                        onMoved: backend.setPadSetting(panel.pad, setting.modelData.name, value)
                    }
                    Label {
                        Layout.preferredWidth: 96
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: theme.colors.muted
                        text: {
                            const v = setting.value
                            switch (setting.modelData.name) {
                            case "volume": return panel.number(v * 100)
                            case "pan": return panel.panText(v)
                            case "filter": return panel.filterText(v)
                            case "resonance": return panel.number(v * 100) + "%"
                            case "attack": return v < 0.03 ? qsTr("At once") : panel.seconds(0.5 * v * v)
                            case "decay": return v >= 0.999 ? qsTr("Rings") : panel.seconds(0.092 * Math.pow(150, v))
                            }
                            return ""
                        }
                    }
                }
            }

            // How it plays: its own swing and loop, and what stops it.
            GridLayout {
                width: parent.width
                columns: 4
                columnSpacing: 6
                rowSpacing: 4

                Label { Layout.preferredWidth: 78; text: qsTr("Swing"); color: theme.colors.muted }
                Chip { implicitHeight: 28; text: "−"; onClicked: backend.changePadSwing(panel.pad, -2) }
                Label {
                    objectName: "padSwing"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.selected.swing > 0 ? panel.number(panel.selected.swing) + "%" : qsTr("The song's")
                    color: panel.selected.swing > 0 ? theme.foreground : theme.colors.muted
                }
                Chip { implicitHeight: 28; text: "+"; onClicked: backend.changePadSwing(panel.pad, 2) }

                Label { Layout.preferredWidth: 78; text: qsTr("Loop"); color: theme.colors.muted }
                Chip { implicitHeight: 28; text: "−"; onClicked: backend.changeLoop(panel.pad, -1) }
                Label {
                    objectName: "padLoop"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.selected.loop > 0 ? qsTr("%Ln steps", "", panel.selected.loop) : qsTr("The pattern's")
                    color: panel.selected.loop > 0 ? theme.foreground : theme.colors.muted
                }
                Chip { implicitHeight: 28; text: "+"; onClicked: backend.changeLoop(panel.pad, 1) }
            }

            Flow {
                width: parent.width
                spacing: 6
                Chip {
                    implicitHeight: 30
                    text: "M"
                    on: panel.selected.mute
                    onColor: theme.colors.red
                    onTextColor: theme.background
                    onClicked: backend.toggleMute(panel.pad)
                }
                Chip {
                    implicitHeight: 30
                    text: "S"
                    on: panel.selected.solo
                    onColor: theme.colors.yellow
                    onTextColor: theme.background
                    onClicked: backend.toggleSolo(panel.pad)
                }
                Chip {
                    objectName: "reverse"
                    implicitHeight: 30
                    text: qsTr("Reverse")
                    on: panel.selected.reverse
                    bold: false
                    onClicked: backend.toggleReverse(panel.pad)
                }
                Chip {
                    objectName: "gate"
                    implicitHeight: 30
                    text: panel.selected.gate ? qsTr("Gate") : qsTr("One shot")
                    on: panel.selected.gate
                    bold: false
                    onClicked: backend.toggleGate(panel.pad)
                }
                Chip {
                    implicitHeight: 30
                    visible: panel.selected.gate || panel.selected.synth !== ""
                    text: qsTr("Note %Ln steps", "", panel.selected.length)
                    bold: false
                    onClicked: backend.cycleLength(panel.pad)
                }
                Chip {
                    implicitHeight: 30
                    text: panel.selected.group > 0
                          ? qsTr("Choke %1").arg(panel.number(panel.selected.group))
                          : qsTr("No choke")
                    bold: false
                    onClicked: backend.cycleGroup(panel.pad)
                }
                Chip { implicitHeight: 30; text: qsTr("Sample…"); bold: false; onClicked: backend.chooseSample(panel.pad) }
                Chip {
                    implicitHeight: 30
                    text: panel.pad < 16 ? qsTr("Kit") : qsTr("Clear")
                    bold: false
                    visible: panel.selected.state !== "kit" && panel.selected.state !== "empty"
                    onClicked: backend.resetPad(panel.pad)
                }
                Chip { implicitHeight: 30; text: qsTr("Chop…"); bold: false; onClicked: panel.chopRequested() }
                Chip {
                    objectName: "padEffects"
                    implicitHeight: 30
                    text: panel.selected.effects > 0 ? qsTr("Effects (%1)").arg(panel.selected.effects) : qsTr("Effects…")
                    on: panel.selected.effects > 0 || panel.selected.synth !== ""
                    bold: false
                    onClicked: panel.effectsRequested()
                }
                Chip {
                    objectName: "padLanes"
                    implicitHeight: 30
                    text: qsTr("Lanes")
                    bold: false
                    onClicked: panel.lanesRequested()
                }
            }
        }
    }
}
