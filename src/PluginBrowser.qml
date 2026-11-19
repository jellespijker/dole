import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The synths or effects to pick from: dole's own first, then every CLAP and
// LV2 plugin found, which the first look for them takes a moment to find.
Popup {
    id: browser

    property bool instrument: false
    property bool rightToLeft: false
    readonly property var choices: backend.plugins.filter(p => p.instrument === instrument)

    signal picked(string id)

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 96 : 600, 620)
    height: Math.min(parent ? parent.height - 96 : 520, 560)
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
    function pick(index) {
        if (index < 0 || index >= choices.length)
            return
        close()
        picked(choices[index].id)
    }

    onOpened: {
        backend.scanPlugins()
        list.currentIndex = 0
        list.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        LayoutMirroring.enabled: browser.rightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: browser.instrument ? qsTr("Play a synth") : qsTr("Add an effect")
                font.bold: true
                font.pixelSize: 18
            }
            Label {
                visible: backend.scanning
                text: qsTr("Looking for plugins…")
                color: theme.colors.muted
            }
        }

        ListView {
            id: list
            objectName: "pluginList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: browser.choices
            keyNavigationEnabled: true
            Keys.onReturnPressed: browser.pick(currentIndex)
            Keys.onEnterPressed: browser.pick(currentIndex)

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                readonly property bool current: ListView.isCurrentItem
                width: ListView.view.width
                height: 44
                radius: Math.min(4, theme.radius)
                color: current ? browser.alpha(theme.accent, 0.25) : browser.alpha(theme.colors.selection, 0.45)
                border.width: current ? 1 : 0
                border.color: theme.accent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 10
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Label {
                        text: row.modelData.vendor === "dole" ? "" : row.modelData.vendor
                        color: theme.colors.muted
                        elide: Text.ElideRight
                        Layout.maximumWidth: 180
                    }
                    Label {
                        text: row.modelData.format === "dole" ? qsTr("Built-in") : row.modelData.format.toUpperCase()
                        color: theme.colors.muted
                        font.pixelSize: 11
                    }
                }
                TapHandler { onTapped: browser.pick(row.index) }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !backend.scanning && (browser.instrument ? browser.choices.length === 0 : browser.choices.length <= 6)
            text: browser.instrument
                  ? qsTr("No synths found. Install CLAP or LV2 synths, such as surge-xt, and they show up here.")
                  : qsTr("Install CLAP or LV2 effects, such as lsp-plugins or guitarix, and they show up here too.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
    }
}
