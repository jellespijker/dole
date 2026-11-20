import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Names.js" as Names

// Every track of a bank side by side, as on a desk: its level, its place,
// mute and solo, and how loud it is right now; then the whole mix.
Popup {
    id: mixer

    property bool rightToLeft: false

    signal effectsRequested(int pad)

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 48 : 1100, 1240)
    height: Math.min(parent ? parent.height - 48 : 640, 620)
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: theme.background
        border.color: theme.colors.selection
        radius: theme.radius
    }

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function number(n) { return n.toLocaleString(Qt.locale(translator.language), 'f', 0) }
    // A meter reads in decibels, from -48 dB at the bottom to 0 at the top.
    function meterHeight(peak) {
        if (!peak || peak <= 0)
            return 0
        return Math.max(0, Math.min(1, 1 + 20 * Math.log(peak) / Math.LN10 / 48))
    }

    onOpened: backend.setMetering(true)
    onClosed: backend.setMetering(false)

    ColumnLayout {
        anchors.fill: parent
        spacing: 14
        LayoutMirroring.enabled: mixer.rightToLeft
        LayoutMirroring.childrenInherit: true

        Keys.onLeftPressed: backend.selectPad(backend.bank * 16 + (backend.selectedPad % 16 + 15) % 16)
        Keys.onRightPressed: backend.selectPad(backend.bank * 16 + (backend.selectedPad % 16 + 1) % 16)
        Keys.onUpPressed: backend.changeVolume(backend.selectedPad, 0.05)
        Keys.onDownPressed: backend.changeVolume(backend.selectedPad, -0.05)
        focus: true

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label {
                Layout.fillWidth: true
                text: qsTr("Mixer")
                font.bold: true
                font.pixelSize: 18
            }
            Label {
                text: qsTr("Bank")
                color: theme.colors.muted
            }
            Repeater {
                model: 4
                delegate: Chip {
                    required property int index
                    implicitHeight: 28
                    text: String.fromCharCode(65 + index)
                    on: backend.bank === index
                    onClicked: backend.setBank(index)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4
            // Tracks go left to right as the pads count, in every language.
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: 16
                delegate: Strip {
                    required property int index
                    pad: backend.bank * 16 + index
                }
            }

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: theme.colors.selection
            }

            // The whole mix, after the master's effects and grit.
            ColumnLayout {
                Layout.fillHeight: true
                Layout.fillWidth: false
                Layout.preferredWidth: 64
                spacing: 6
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Mix")
                    font.bold: true
                    font.pixelSize: 12
                }
                Item {
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    Rectangle {
                        objectName: "masterMeter"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 14
                        height: parent.height
                        radius: Math.min(3, theme.radius)
                        color: mixer.alpha(theme.colors.selection, 0.5)
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: parent.height * mixer.meterHeight(backend.meters[64])
                            radius: parent.radius
                            color: (backend.meters[64] || 0) > 0.98 ? theme.colors.red : theme.accent
                        }
                    }
                }
                Chip {
                    Layout.alignment: Qt.AlignHCenter
                    implicitHeight: 28
                    text: qsTr("FX")
                    bold: false
                    onClicked: mixer.effectsRequested(-1)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("← → choose a track, ↑ ↓ its level. Double-tap a fader or a pan to put it back.")
            color: theme.colors.muted
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }

    // One track: its name, its pan, its fader beside its meter, and its
    // mute, solo, and effects.
    component Strip: Rectangle {
        id: strip
        property int pad: 0
        readonly property var info: backend.pads[pad]
        readonly property bool chosen: backend.selectedPad === pad

        objectName: "strip" + pad
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 44
        Layout.preferredWidth: 64
        radius: Math.min(4, theme.radius)
        color: chosen ? mixer.alpha(theme.accent, 0.14) : mixer.alpha(theme.colors.selection, 0.25)
        border.width: chosen ? 1 : 0
        border.color: theme.accent
        opacity: info.audible ? 1 : 0.5

        TapHandler { onTapped: backend.selectPad(strip.pad) }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 4

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: strip.info.name
                color: strip.chosen ? theme.accent : theme.colors.muted
                font.pixelSize: 10
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: Names.pad(strip.info)
                color: strip.info.state === "empty" ? theme.colors.muted : theme.foreground
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            Slider {
                objectName: "pan" + strip.pad
                Layout.fillWidth: true
                implicitHeight: 26
                from: -1
                to: 1
                value: strip.info.pan
                focusPolicy: Qt.NoFocus
                onMoved: backend.setPadSetting(strip.pad, "pan", value)
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: backend.setPadSetting(strip.pad, "pan", 0)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 2
                Slider {
                    objectName: "fader" + strip.pad
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    orientation: Qt.Vertical
                    from: 0
                    to: 1
                    value: strip.info.volume
                    focusPolicy: Qt.NoFocus
                    onMoved: backend.setPadSetting(strip.pad, "volume", value)
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onDoubleTapped: backend.setPadSetting(strip.pad, "volume", 0.8)
                    }
                }
                Rectangle {
                    objectName: "meter" + strip.pad
                    Layout.fillHeight: true
                    implicitWidth: 6
                    radius: Math.min(3, theme.radius)
                    color: mixer.alpha(theme.colors.selection, 0.5)
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: parent.height * mixer.meterHeight(backend.meters[strip.pad])
                        radius: parent.radius
                        color: theme.accent
                    }
                }
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: mixer.number(strip.info.volume * 100)
                color: theme.colors.muted
                font.pixelSize: 10
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 2
                Chip {
                    implicitHeight: 24
                    implicitWidth: 24
                    text: "M"
                    on: strip.info.mute
                    onColor: theme.colors.red
                    onTextColor: theme.background
                    onClicked: backend.toggleMute(strip.pad)
                }
                Chip {
                    implicitHeight: 24
                    implicitWidth: 24
                    text: "S"
                    on: strip.info.solo
                    onColor: theme.colors.yellow
                    onTextColor: theme.background
                    onClicked: backend.toggleSolo(strip.pad)
                }
            }
            Chip {
                Layout.alignment: Qt.AlignHCenter
                implicitHeight: 24
                text: qsTr("FX")
                on: strip.info.effects > 0 || strip.info.synth !== ""
                bold: false
                onClicked: mixer.effectsRequested(strip.pad)
            }
        }
    }
}
