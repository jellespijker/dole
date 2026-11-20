import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Names.js" as Names

// The sixteen pads of a bank, printed on the accent stock, four by four with
// pad 1 at the bottom left as on an MPC; under them the settings of the pad
// last hit. Each pad answers its own finger, so several play at once on a
// touch screen.
ColumnLayout {
    id: panel

    signal chopRequested()
    signal effectsRequested()
    signal lanesRequested()

    spacing: 0

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
    function sourceText(p) {
        return p.synth !== "" ? qsTr("Synth")
             : p.state === "kit" ? qsTr("Kit")
             : p.state === "empty" ? qsTr("Empty")
             : p.state === "loading" ? qsTr("Loading…")
             : p.state === "failed" ? qsTr("Can't load")
             : p.chop ? qsTr("Chop") : qsTr("Sample")
    }

    readonly property int pad: backend.selectedPad
    readonly property var selected: backend.pads[pad]
    // How tall a pad may be: a little less than square, and short enough on
    // a low window to leave the settings room.
    property real padHeight: 80

    // The accent stock: the one coloured sheet on the board.
    Rectangle {
        id: stock
        Layout.fillWidth: true
        implicitHeight: stockColumn.implicitHeight + 34
        color: theme.accent

        ColumnLayout {
            id: stockColumn
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.topMargin: 14
            anchors.bottomMargin: 20
            spacing: 12

            // Banks A to D, and note repeat beside them.
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    visible: panel.width > 420
                    text: qsTr("Bank")
                    font.family: theme.displayFont
                    font.pixelSize: 28
                    font.capitalization: Font.AllUppercase
                    color: theme.accentForeground
                    Layout.rightMargin: 8
                }
                Repeater {
                    model: 4
                    delegate: Chip {
                        required property int index
                        objectName: "bank" + index
                        implicitWidth: 36
                        Layout.fillWidth: true
                        Layout.maximumWidth: 46
                        text: String.fromCharCode(65 + index)
                        ink: theme.accentForeground
                        onColor: theme.accentForeground
                        onTextColor: theme.accent
                        on: backend.bank === index
                        onClicked: backend.setBank(index)
                    }
                }
                Item { Layout.fillWidth: true }
                Chip {
                    objectName: "repeat"
                    text: qsTr("Repeat %1").arg(backend.repeatName)
                    ink: theme.accentForeground
                    onColor: theme.accentForeground
                    onTextColor: theme.accent
                    on: backend.repeat > 0
                    onClicked: backend.toggleRepeat()
                    onPressAndHold: backend.cycleRepeat()
                }
            }

            GridLayout {
                id: pads
                Layout.fillWidth: true
                Layout.preferredHeight: panel.padHeight * 4 + rowSpacing * 3
                columns: 4
                rowSpacing: 10
                columnSpacing: 10
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
                        readonly property bool empty: info.state === "empty"
                        // A hit prints the pad solid for a moment.
                        property real glow: 0
                        readonly property bool lit: glow > 0.4
                        readonly property color ink: lit ? theme.background : empty ? theme.accentForeground : theme.foreground

                        objectName: "pad" + number
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Math.min(4, theme.radius)
                        color: empty && glow === 0 ? "transparent"
                             : Qt.tint(empty ? theme.accent : theme.background, panel.alpha(theme.foreground, glow))

                        DashedBox {
                            anchors.fill: parent
                            visible: pad.empty
                            color: panel.alpha(theme.accentForeground, 0.7)
                        }
                        // The pad chosen: ringed in the stock's ink.
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -7
                            visible: pad.chosen
                            color: "transparent"
                            radius: parent.radius
                            border.width: 4
                            border.color: theme.accentForeground
                        }

                        Label {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 8
                            text: pad.info.name
                            font.pixelSize: 12
                            font.bold: true
                            color: pad.ink
                        }
                        Label {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 8
                            text: Names.keys[pad.inBank]
                            font.pixelSize: 12
                            font.bold: true
                            color: pad.ink
                        }
                        // The pad runs through effects, or plays a synth.
                        Label {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 8
                            visible: pad.info.effects > 0 || pad.info.synth !== ""
                            text: pad.info.synth !== "" ? qsTr("Synth") : qsTr("FX")
                            font.pixelSize: 11
                            font.bold: true
                            font.capitalization: Font.AllUppercase
                            color: pad.lit ? theme.background : theme.accent
                        }
                        Label {
                            id: padName
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 8
                            anchors.bottomMargin: 6
                            anchors.rightMargin: pad.info.effects > 0 || pad.info.synth !== "" ? 40 : 8
                            text: Names.pad(pad.info)
                            elide: Text.ElideRight
                            font.family: theme.displayFont
                            font.pixelSize: Math.max(15, Math.min(26, pad.height * 0.3))
                            font.capitalization: Font.AllUppercase
                            color: pad.info.state === "failed" && !pad.lit ? theme.colors.red
                                 : !pad.info.audible && !pad.lit ? panel.alpha(pad.ink, 0.5) : pad.ink
                        }
                        Stamp {
                            visible: pad.info.mute
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: 6
                            size: 15
                            text: qsTr("Mute")
                        }

                        NumberAnimation on glow {
                            id: flash
                            running: false
                            from: 1
                            to: 0
                            duration: 240
                            easing.type: Easing.OutQuad
                        }
                        Connections {
                            target: backend
                            function onPadHit(hit) {
                                if (hit === pad.number)
                                    flash.restart()
                            }
                        }

                        // A pad gives a little under the finger.
                        scale: press.pressed && theme.motion ? 0.95 : 1
                        Behavior on scale { NumberAnimation { duration: 70; easing.type: Easing.OutCubic } }

                        // Sounds on the press, not the release, and repeats while held.
                        TapHandler {
                            id: press
                            gesturePolicy: TapHandler.WithinBounds
                            onPressedChanged: pressed ? backend.padDown(pad.number) : backend.padUp(pad.number)
                        }
                    }
                }
            }
        }
    }

    // The pad last hit: its name as a headline, then its sound, its place,
    // and its timing, scrolled on a short window.
    Flickable {
        id: settings
        objectName: "padSettings"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentHeight: settingsColumn.height + 32
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: settingsColumn
            x: 20
            y: 14
            width: settings.width - 40
            spacing: 8

            RowLayout {
                width: parent.width
                spacing: 12
                Label {
                    objectName: "padTitle"
                    Layout.fillWidth: true
                    text: panel.selected.name + " " + Names.pad(panel.selected)
                    font.family: theme.displayFont
                    font.pixelSize: 34
                    font.capitalization: Font.AllUppercase
                    elide: Text.ElideRight
                }
                Label {
                    text: panel.sourceText(panel.selected)
                          + (panel.selected.effects > 0 ? " · " + qsTr("%Ln effects", "", panel.selected.effects) : "")
                    color: panel.selected.state === "failed" ? theme.colors.red : theme.quiet
                    font.pixelSize: 12
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.8
                }
            }

            component SettingLabel: Label {
                Layout.preferredWidth: 96
                color: theme.foreground
                font.pixelSize: 12
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.7
                elide: Text.ElideRight
            }

            RowLayout {
                width: parent.width
                spacing: 8
                SettingLabel { text: qsTr("Tune") }
                Chip { implicitWidth: 40; text: "−"; onClicked: backend.changeTune(panel.pad, -1) }
                Label {
                    objectName: "tune"
                    Layout.minimumWidth: 40
                    horizontalAlignment: Text.AlignHCenter
                    text: (panel.selected.tune > 0 ? "+" : "") + panel.number(panel.selected.tune)
                    font.family: theme.displayFont
                    font.pixelSize: 22
                }
                Chip { implicitWidth: 40; text: "+"; onClicked: backend.changeTune(panel.pad, 1) }
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
                    spacing: 8
                    SettingLabel { text: setting.modelData.label }
                    Slider {
                        id: slider
                        objectName: "setting_" + setting.modelData.name
                        Layout.fillWidth: true
                        implicitHeight: 34
                        from: setting.modelData.from
                        to: setting.modelData.to
                        value: setting.value
                        focusPolicy: Qt.NoFocus
                        // Left is left in every language.
                        LayoutMirroring.enabled: false
                        onMoved: backend.setPadSetting(panel.pad, setting.modelData.name, value)

                        // A printed rule, inked from its rest point to the value,
                        // and a solid block to take hold of.
                        readonly property real rest: from < 0 ? 0.5 : 0
                        background: Item {
                            x: slider.leftPadding
                            width: slider.availableWidth
                            height: slider.height
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width
                                height: 2
                                color: panel.alpha(theme.foreground, 0.3)
                            }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                x: parent.width * Math.min(slider.rest, slider.visualPosition)
                                width: parent.width * Math.abs(slider.visualPosition - slider.rest)
                                height: 3
                                color: theme.foreground
                            }
                        }
                        handle: Rectangle {
                            x: slider.leftPadding + slider.visualPosition * slider.availableWidth - width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 22
                            color: slider.pressed ? theme.accent : theme.foreground
                        }
                    }
                    Label {
                        Layout.preferredWidth: 104
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        font.pixelSize: 13
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
                columnSpacing: 8
                rowSpacing: 8

                SettingLabel { text: qsTr("Swing") }
                Chip { implicitWidth: 40; text: "−"; onClicked: backend.changePadSwing(panel.pad, -2) }
                Label {
                    objectName: "padSwing"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.selected.swing > 0 ? panel.number(panel.selected.swing) + "%" : qsTr("The song's")
                    color: panel.selected.swing > 0 ? theme.foreground : theme.quiet
                }
                Chip { implicitWidth: 40; text: "+"; onClicked: backend.changePadSwing(panel.pad, 2) }

                SettingLabel { text: qsTr("Loop") }
                Chip { implicitWidth: 40; text: "−"; onClicked: backend.changeLoop(panel.pad, -1) }
                Label {
                    objectName: "padLoop"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.selected.loop > 0 ? qsTr("%Ln steps", "", panel.selected.loop) : qsTr("The pattern's")
                    color: panel.selected.loop > 0 ? theme.foreground : theme.quiet
                }
                Chip { implicitWidth: 40; text: "+"; onClicked: backend.changeLoop(panel.pad, 1) }
            }

            Flow {
                width: parent.width
                spacing: 8
                Chip {
                    text: qsTr("Mute")
                    on: panel.selected.mute
                    onColor: theme.colors.red
                    onTextColor: theme.background
                    onClicked: backend.toggleMute(panel.pad)
                }
                Chip {
                    text: qsTr("Solo")
                    on: panel.selected.solo
                    onColor: theme.colors.yellow
                    onTextColor: theme.background
                    onClicked: backend.toggleSolo(panel.pad)
                }
                Chip {
                    objectName: "reverse"
                    text: qsTr("Reverse")
                    on: panel.selected.reverse
                    onClicked: backend.toggleReverse(panel.pad)
                }
                Chip {
                    objectName: "gate"
                    text: panel.selected.gate ? qsTr("Gate") : qsTr("One shot")
                    on: panel.selected.gate
                    onClicked: backend.toggleGate(panel.pad)
                }
                Chip {
                    visible: panel.selected.gate || panel.selected.synth !== ""
                    text: qsTr("Note %Ln steps", "", panel.selected.length)
                    onClicked: backend.cycleLength(panel.pad)
                }
                Chip {
                    text: panel.selected.group > 0
                          ? qsTr("Choke %1").arg(panel.number(panel.selected.group))
                          : qsTr("No choke")
                    on: panel.selected.group > 0
                    onClicked: backend.cycleGroup(panel.pad)
                }
                Chip { text: qsTr("Sample…"); onClicked: backend.chooseSample(panel.pad) }
                Chip {
                    text: panel.pad < 16 ? qsTr("Kit") : qsTr("Clear")
                    visible: panel.selected.state !== "kit" && panel.selected.state !== "empty"
                    onClicked: backend.resetPad(panel.pad)
                }
                Chip { text: qsTr("Chop…"); onClicked: panel.chopRequested() }
                Chip {
                    objectName: "padEffects"
                    text: panel.selected.effects > 0 ? qsTr("Effects (%1)").arg(panel.selected.effects) : qsTr("Effects…")
                    on: panel.selected.effects > 0 || panel.selected.synth !== ""
                    onClicked: panel.effectsRequested()
                }
                Chip {
                    objectName: "padLanes"
                    text: qsTr("Lanes")
                    onClicked: panel.lanesRequested()
                }
            }
        }
    }
}
