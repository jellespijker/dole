import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Names.js" as Names

// What a pad (or the master) plays through: a synth, if the pad has one,
// then its effects in order, each with its controls as sliders and, where
// the plugin has one, its own window.
Popup {
    id: view

    property bool rightToLeft: false
    // -1 is the synth; 0 and up the effects.
    property int selected: 0
    readonly property var rack: backend.rack
    readonly property int target: backend.rackTarget
    readonly property bool isPad: target >= 0
    readonly property var effects: rack.effects || []
    readonly property var current: selected < 0 ? (rack.synth || null) : (effects[selected] || null)

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 48 : 900, 1040)
    height: Math.min(parent ? parent.height - 48 : 600, 660)
    modal: true
    focus: true
    padding: 22
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: theme.background
        border.width: 3
        border.color: theme.foreground
        radius: theme.radius
    }

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    function openFor(target) {
        backend.setRackTarget(target)
        selected = target >= 0 && backend.rack.synth ? -1 : 0
        open()
    }

    // Up and down through the synth and the effects.
    function move(delta) {
        const first = isPad && rack.synth ? -1 : 0
        const last = Math.max(first, effects.length - 1)
        selected = Math.max(first, Math.min(last, selected + delta))
    }

    function browse(instrument) {
        browser.instrument = instrument
        browser.open()
    }

    onSelectedChanged: find.text = ""

    onEffectsChanged: if (selected >= effects.length && selected >= 0) selected = Math.max(isPad && rack.synth ? -1 : 0, effects.length - 1)

    ColumnLayout {
        anchors.fill: parent
        spacing: 16
        focus: true
        // A popup is not an Item and doesn't inherit the window's mirroring,
        // so its content opts in.
        LayoutMirroring.enabled: view.rightToLeft
        LayoutMirroring.childrenInherit: true

        Keys.onUpPressed: view.move(-1)
        Keys.onDownPressed: view.move(1)
        Keys.onDeletePressed: if (view.current) backend.removeEffect(view.target, view.selected)
        Keys.onSpacePressed: if (view.current) backend.toggleBypass(view.target, view.selected)

        Label {
            objectName: "chainTitle"
            text: view.isPad ? qsTr("Pad %1").arg(backend.pads[view.target].name) + "  " + Names.pad(backend.pads[view.target])
                             : qsTr("Master effects")
            font.bold: true
            font.pixelSize: 18
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 22

            // The chain, top to bottom as the sound goes.
            ColumnLayout {
                Layout.preferredWidth: 300
                Layout.maximumWidth: 300
                Layout.fillHeight: true
                spacing: 8

                Label {
                    visible: view.isPad
                    text: qsTr("Synth")
                    color: theme.colors.muted
                }
                SlotRow {
                    visible: view.isPad && !!view.rack.synth
                    slot: view.rack.synth || null
                    position: -1
                }
                RowLayout {
                    visible: view.isPad
                    spacing: 6
                    Chip {
                        objectName: "synthButton"
                        text: view.rack.synth ? qsTr("Change synth…") : qsTr("Play a synth…")
                        bold: false
                        onClicked: view.browse(true)
                    }
                    Chip {
                        visible: !!view.rack.synth
                        text: qsTr("Note %Ln steps", "", view.rack.length || 1)
                        bold: false
                        onClicked: backend.cycleLength(view.target)
                    }
                }

                Label {
                    Layout.topMargin: view.isPad ? 8 : 0
                    text: qsTr("Effects")
                    color: theme.colors.muted
                }
                Repeater {
                    model: view.effects
                    delegate: SlotRow {
                        required property var modelData
                        required property int index
                        slot: modelData
                        position: index
                    }
                }
                Chip {
                    objectName: "addEffect"
                    visible: view.effects.length < 4
                    text: qsTr("Add effect…")
                    bold: false
                    onClicked: view.browse(false)
                }
                Item { Layout.fillHeight: true }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("↑ ↓ choose, Space turns on or off, Delete removes.")
                    color: theme.colors.muted
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
            }

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: theme.colors.selection
            }

            // The chosen plugin's controls.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    visible: !!view.current
                    Label {
                        Layout.fillWidth: true
                        text: view.current ? view.current.name : ""
                        font.bold: true
                        font.pixelSize: 16
                        elide: Text.ElideRight
                    }
                    DialogButton {
                        objectName: "editorButton"
                        visible: !!view.current && view.current.editor
                        label: qsTr("&Open its window")
                        onClicked: backend.openEditor(view.target, view.selected)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: !!view.current && view.current.error !== ""
                    text: view.current ? qsTr("Couldn't load it: %1").arg(view.current.error) : ""
                    color: theme.colors.red
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: !view.current
                    text: view.isPad ? qsTr("Add an effect to dirty this pad up, or play a synth on it.")
                                     : qsTr("Add an effect to run the whole mix through.")
                    color: theme.colors.muted
                    wrapMode: Text.Wrap
                }

                // A big synth has hundreds of controls; find one by name.
                TextField {
                    id: find
                    objectName: "findControl"
                    Layout.fillWidth: true
                    visible: !!view.current && view.current.params.length > 12
                    placeholderText: qsTr("Find a control")
                    Material.roundedScale: theme.radius
                    Keys.onEscapePressed: (event) => {
                        if (text !== "") {
                            text = ""
                            event.accepted = true
                        } else {
                            event.accepted = false
                        }
                    }
                }

                ListView {
                    id: params
                    objectName: "params"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: {
                        const all = view.current ? view.current.params : []
                        const wanted = find.visible ? find.text.trim().toLowerCase() : ""
                        return wanted === "" ? all : all.filter(p => p.name.toLowerCase().indexOf(wanted) >= 0)
                    }
                    ScrollBar.vertical: ScrollBar {}

                    delegate: RowLayout {
                        id: param
                        required property var modelData
                        width: ListView.view.width - 12
                        spacing: 10

                        // A logarithmic control slides evenly through octaves, not hertz.
                        function toValue(t) {
                            return modelData.log ? modelData.min * Math.pow(modelData.max / modelData.min, t)
                                                 : modelData.min + t * (modelData.max - modelData.min)
                        }
                        function toPosition(v) {
                            return modelData.log ? Math.log(v / modelData.min) / Math.log(modelData.max / modelData.min)
                                                 : (v - modelData.min) / Math.max(1e-9, modelData.max - modelData.min)
                        }

                        Label {
                            Layout.preferredWidth: 140
                            text: param.modelData.name
                            elide: Text.ElideRight
                        }
                        Slider {
                            objectName: "param_" + param.modelData.id
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            stepSize: param.modelData.stepped ? 1 / Math.max(1, param.modelData.max - param.modelData.min) : 0
                            snapMode: param.modelData.stepped ? Slider.SnapAlways : Slider.NoSnap
                            value: param.toPosition(param.modelData.value)
                            focusPolicy: Qt.NoFocus
                            onMoved: valueText.text = backend.setParameter(view.target, view.selected, param.modelData.id,
                                                                           param.toValue(value))
                        }
                        Label {
                            id: valueText
                            Layout.preferredWidth: 80
                            horizontalAlignment: Text.AlignRight
                            text: param.modelData.text
                            color: theme.colors.muted
                        }
                    }
                }
            }
        }
    }

    // One plugin in the chain: tap to see its controls.
    component SlotRow: Rectangle {
        id: row
        property var slot: null
        property int position: 0
        readonly property bool chosen: view.selected === position

        Layout.fillWidth: true
        implicitHeight: 40
        radius: Math.min(4, theme.radius)
        color: chosen ? view.alpha(theme.accent, 0.22) : view.alpha(theme.colors.selection, 0.45)
        border.width: chosen ? 1 : 0
        border.color: theme.accent
        opacity: row.slot && row.slot.bypass ? 0.55 : 1

        TapHandler { onTapped: view.selected = row.position }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 6
            spacing: 4
            Label {
                Layout.fillWidth: true
                text: row.slot ? row.slot.name : ""
                color: row.slot && row.slot.error !== "" ? theme.colors.red : theme.foreground
                font.bold: row.chosen
                elide: Text.ElideRight
            }
            Label {
                text: row.slot ? (row.slot.format === "dole" ? "" : row.slot.format.toUpperCase()) : ""
                color: theme.colors.muted
                font.pixelSize: 10
            }
            Chip {
                implicitHeight: 26
                text: row.slot && row.slot.bypass ? qsTr("Off") : qsTr("On")
                on: !!row.slot && !row.slot.bypass
                bold: false
                onClicked: backend.toggleBypass(view.target, row.position)
            }
            Chip {
                visible: row.position >= 0
                implicitHeight: 26
                text: "↑"
                onClicked: {
                    backend.moveEffect(view.target, row.position, -1)
                    view.selected = Math.max(0, row.position - 1)
                }
            }
            Chip {
                visible: row.position >= 0
                implicitHeight: 26
                text: "↓"
                onClicked: {
                    backend.moveEffect(view.target, row.position, 1)
                    view.selected = Math.min(view.effects.length - 1, row.position + 1)
                }
            }
            Chip {
                implicitHeight: 26
                text: "✕"
                onClicked: backend.removeEffect(view.target, row.position)
            }
        }
    }

    PluginBrowser {
        id: browser
        objectName: "browser"
        rightToLeft: view.rightToLeft
        onPicked: (id) => {
            if (instrument) {
                backend.setSynth(view.target, id)
                view.selected = -1
            } else {
                backend.addEffect(view.target, id)
                view.selected = view.effects.length - 1
            }
        }
    }
}
