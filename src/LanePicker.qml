import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What a track's new lane can move: its own pitch, volume, place, filter,
// and decay, or any parameter of its synth and effects.
Popup {
    id: picker

    property int pad: 0
    property bool rightToLeft: false
    property var choices: []
    readonly property var shown: {
        const wanted = find.text.trim().toLowerCase()
        return wanted === "" ? choices
                             : choices.filter(c => (c.name + " " + c.group).toLowerCase().indexOf(wanted) >= 0)
    }

    signal picked(string id)

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 96 : 560, 560)
    height: Math.min(parent ? parent.height - 96 : 520, 560)
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: theme.background
        border.width: 3
        border.color: theme.foreground
        radius: theme.radius
    }

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function openFor(p) {
        pad = p
        choices = backend.laneChoices(p)
        find.text = ""
        open()
    }
    function pick(index) {
        if (index < 0 || index >= shown.length)
            return
        close()
        picked(shown[index].id)
    }

    onOpened: {
        list.currentIndex = 0
        find.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        LayoutMirroring.enabled: picker.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            text: qsTr("Add a lane")
            font.bold: true
            font.pixelSize: 18
        }
        TextField {
            id: find
            objectName: "findLane"
            Layout.fillWidth: true
            placeholderText: qsTr("Find a setting")
            Material.roundedScale: theme.radius
            Keys.onDownPressed: list.incrementCurrentIndex()
            Keys.onUpPressed: list.decrementCurrentIndex()
            Keys.onReturnPressed: picker.pick(list.currentIndex)
            Keys.onEnterPressed: picker.pick(list.currentIndex)
        }

        ListView {
            id: list
            objectName: "laneList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: picker.shown
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                readonly property bool current: ListView.isCurrentItem
                width: ListView.view.width - 10
                height: 40
                radius: Math.min(4, theme.radius)
                color: current ? picker.alpha(theme.accent, 0.25) : picker.alpha(theme.colors.selection, 0.45)
                border.width: current ? 1 : 0
                border.color: theme.accent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.maximumWidth: 200
                        text: row.modelData.group
                        color: theme.colors.muted
                        elide: Text.ElideRight
                    }
                }
                TapHandler { onTapped: picker.pick(row.index) }
            }
        }
        Label {
            Layout.fillWidth: true
            // Only the track's own settings: no plugins on it yet.
            visible: picker.choices.length <= 6
            text: qsTr("Add a synth or effects to the track and their settings can have lanes too.")
            color: theme.colors.muted
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
