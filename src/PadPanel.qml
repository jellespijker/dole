import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Names.js" as Names

// The sixteen pads, four by four with pad 1 at the bottom left as on an
// MPC, and the settings of the pad last hit. Each pad answers its own
// finger, so several can be played at once on a touch screen.
ColumnLayout {
    id: panel

    signal chopRequested()
    signal effectsRequested()

    spacing: 12

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    readonly property var selected: backend.pads[backend.selectedPad]

    GridLayout {
        id: pads
        Layout.fillWidth: true
        Layout.preferredHeight: width
        columns: 4
        rowSpacing: 8
        columnSpacing: 8
        // Pads sit where an MPC has them in every language.
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true

        Repeater {
            model: 16

            delegate: Rectangle {
                id: pad
                required property int index
                // Top row first: 13 to 16, down to 1 to 4.
                readonly property int number: (3 - Math.floor(index / 4)) * 4 + index % 4
                readonly property var info: backend.pads[number]
                readonly property bool chosen: backend.selectedPad === number
                property real glow: 0

                objectName: "pad" + number
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Math.min(8, theme.radius)
                color: Qt.tint(panel.alpha(theme.colors.selection, 0.85), panel.alpha(theme.accent, glow * 0.85))
                border.width: chosen ? 2 : 1
                border.color: chosen ? theme.accent : theme.colors.selection
                opacity: info.audible ? 1 : 0.45

                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 7
                    text: (pad.number + 1).toLocaleString(Qt.locale(translator.language), 'f', 0)
                    font.pixelSize: 11
                    font.bold: true
                    color: pad.chosen ? theme.accent : theme.colors.muted
                }
                Label {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 7
                    text: Names.keys[pad.number]
                    font.pixelSize: 10
                    color: theme.colors.muted
                }
                // The pad runs through effects, or plays a synth.
                Label {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 7
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
                    anchors.margins: 7
                    text: Names.pad(pad.info)
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    font.bold: true
                    color: pad.info.state === "failed" ? theme.colors.red : theme.foreground
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
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                objectName: "padTitle"
                Layout.fillWidth: true
                text: qsTr("Pad %1").arg((backend.selectedPad + 1).toLocaleString(Qt.locale(translator.language), 'f', 0))
                      + "  " + Names.pad(panel.selected)
                font.bold: true
                font.pixelSize: 15
                elide: Text.ElideRight
            }
            Label {
                text: panel.selected.synth !== "" ? qsTr("Synth")
                      : panel.selected.state === "kit" ? qsTr("Kit")
                      : panel.selected.state === "loading" ? qsTr("Loading…")
                      : panel.selected.state === "failed" ? qsTr("Can't load")
                      : panel.selected.chop ? qsTr("Chop") : qsTr("Sample")
                color: panel.selected.state === "failed" ? theme.colors.red : theme.colors.muted
                font.pixelSize: 12
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 6
            rowSpacing: 6

            Label { text: qsTr("Tune"); color: theme.colors.muted }
            Chip { text: "−"; onClicked: backend.changeTune(backend.selectedPad, -1) }
            Label {
                objectName: "tune"
                Layout.minimumWidth: 36
                horizontalAlignment: Text.AlignHCenter
                text: (panel.selected.tune > 0 ? "+" : "") + panel.selected.tune.toLocaleString(Qt.locale(translator.language), 'f', 0)
                font.bold: true
            }
            Chip { text: "+"; onClicked: backend.changeTune(backend.selectedPad, 1) }

            Label { text: qsTr("Volume"); color: theme.colors.muted }
            Chip { text: "−"; onClicked: backend.changeVolume(backend.selectedPad, -0.05) }
            Label {
                Layout.minimumWidth: 36
                horizontalAlignment: Text.AlignHCenter
                text: Math.round(panel.selected.volume * 100).toLocaleString(Qt.locale(translator.language), 'f', 0)
                font.bold: true
            }
            Chip { text: "+"; onClicked: backend.changeVolume(backend.selectedPad, 0.05) }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            Chip {
                text: "M"
                on: panel.selected.mute
                onColor: theme.colors.red
                onTextColor: theme.background
                onClicked: backend.toggleMute(backend.selectedPad)
            }
            Chip {
                text: "S"
                on: panel.selected.solo
                onColor: theme.colors.yellow
                onTextColor: theme.background
                onClicked: backend.toggleSolo(backend.selectedPad)
            }
            Chip {
                text: panel.selected.group > 0
                      ? qsTr("Group %1").arg(panel.selected.group.toLocaleString(Qt.locale(translator.language), 'f', 0))
                      : qsTr("No group")
                bold: false
                onClicked: backend.cycleGroup(backend.selectedPad)
            }
            Chip { text: qsTr("Sample…"); bold: false; onClicked: backend.chooseSample(backend.selectedPad) }
            Chip {
                text: qsTr("Kit")
                bold: false
                visible: panel.selected.state !== "kit"
                onClicked: backend.resetPad(backend.selectedPad)
            }
            Chip { text: qsTr("Chop…"); bold: false; onClicked: panel.chopRequested() }
            Chip {
                objectName: "padEffects"
                text: panel.selected.effects > 0 ? qsTr("Effects (%1)").arg(panel.selected.effects) : qsTr("Effects…")
                on: panel.selected.effects > 0 || panel.selected.synth !== ""
                bold: false
                onClicked: panel.effectsRequested()
            }
        }
    }
}
