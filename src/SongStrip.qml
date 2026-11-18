import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The song: the chain of patterns it plays, one bar each, with a slot at
// the end to add the next.
ColumnLayout {
    id: strip

    property bool active: false
    property int cursor: 0

    signal slotPicked(int index)

    spacing: 6

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    RowLayout {
        spacing: 10
        Label {
            text: qsTr("Song")
            font.bold: true
            color: strip.active ? theme.accent : theme.foreground
        }
        Label {
            objectName: "songLength"
            text: qsTr("%Ln bars", "", backend.chain.length)
            color: theme.colors.muted
            font.pixelSize: 12
        }
        Label {
            visible: !strip.active
            text: qsTr("Tab to edit")
            color: theme.colors.muted
            font.pixelSize: 12
        }
    }

    Flickable {
        id: flick
        Layout.fillWidth: true
        implicitHeight: 34
        contentWidth: slots.width
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Like the steps, the song runs left to right in every language.
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true

        Row {
            id: slots
            spacing: 4

            Repeater {
                model: backend.chain
                delegate: Rectangle {
                    required property int index
                    required property int modelData
                    readonly property bool playing: backend.playing && backend.songMode && backend.playChainIndex === index
                    objectName: "slot" + index
                    width: 30
                    height: 30
                    radius: Math.min(4, theme.radius)
                    color: playing ? theme.accent : strip.alpha(theme.colors.selection, 0.8)
                    border.width: strip.active && strip.cursor === index ? 2 : 0
                    border.color: theme.foreground
                    Label {
                        anchors.centerIn: parent
                        text: String.fromCharCode(65 + parent.modelData)
                        font.bold: true
                        color: parent.playing ? theme.accentForeground : theme.foreground
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: strip.slotPicked(parent.index)
                    }
                }
            }

            // Where the next pattern goes.
            Rectangle {
                width: 30
                height: 30
                radius: Math.min(4, theme.radius)
                color: "transparent"
                border.width: strip.active && strip.cursor === backend.chain.length ? 2 : 1
                border.color: strip.active && strip.cursor === backend.chain.length ? theme.foreground : theme.colors.muted
                Label {
                    anchors.centerIn: parent
                    text: "+"
                    color: theme.colors.muted
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: strip.slotPicked(backend.chain.length)
                }
            }
        }
    }

    // Keeps the cursor, or the slot playing, in view.
    function reveal(index) {
        const left = index * 34, right = left + 30
        if (left < flick.contentX)
            flick.contentX = left
        else if (right > flick.contentX + flick.width)
            flick.contentX = right - flick.width
    }
    onCursorChanged: reveal(cursor)
    Connections {
        target: backend
        function onPositionChanged() {
            if (backend.songMode && backend.playChainIndex >= 0)
                strip.reveal(backend.playChainIndex)
        }
    }
}
