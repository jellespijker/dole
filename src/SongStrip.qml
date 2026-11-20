import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The song along the bottom: whether Space plays the pattern or the song,
// the chain of patterns it plays, with a slot at the end for the next, and
// what just happened.
RowLayout {
    id: strip

    property bool active: false
    property int cursor: 0

    signal slotPicked(int index)

    spacing: 10

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // The word inverts while the keys edit the song.
    Rectangle {
        implicitWidth: songWord.implicitWidth + 16
        implicitHeight: 40
        color: strip.active ? theme.foreground : "transparent"
        Label {
            id: songWord
            anchors.centerIn: parent
            text: qsTr("Song")
            font.family: theme.displayFont
            font.pixelSize: 28
            font.capitalization: Font.AllUppercase
            color: strip.active ? theme.background : theme.foreground
        }
        TapHandler { onTapped: strip.slotPicked(strip.cursor) }
    }

    // What Space plays: the pattern being edited, or the song.
    Row {
        objectName: "mode"
        Repeater {
            model: [qsTr("Pattern"), qsTr("Song")]
            delegate: Chip {
                required property int index
                required property string modelData
                text: modelData
                implicitHeight: 34
                on: (index === 1) === backend.songMode
                onColor: theme.foreground
                onTextColor: theme.background
                onClicked: if (!on) backend.toggleSongMode()
            }
        }
    }

    Flickable {
        id: flick
        Layout.fillWidth: true
        Layout.preferredWidth: slots.width
        Layout.maximumWidth: slots.width
        Layout.leftMargin: 6
        implicitHeight: 48
        contentWidth: slots.width
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Like the steps, the song runs left to right in every language.
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true

        Row {
            id: slots
            spacing: 6
            anchors.verticalCenter: parent.verticalCenter

            Repeater {
                model: backend.chain
                delegate: Rectangle {
                    required property int index
                    required property int modelData
                    readonly property bool playing: backend.playing && backend.songMode && backend.playChainIndex === index
                    readonly property bool cursor: strip.active && strip.cursor === index
                    objectName: "slot" + index
                    width: 46
                    height: 40
                    color: playing ? theme.foreground : "transparent"
                    border.width: cursor ? 3 : 2
                    border.color: cursor ? theme.accent : theme.foreground
                    Label {
                        anchors.centerIn: parent
                        text: String.fromCharCode(65 + parent.modelData)
                        font.family: theme.displayFont
                        font.pixelSize: 22
                        color: parent.playing ? theme.background : theme.foreground
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: strip.slotPicked(parent.index)
                    }
                }
            }

            // Where the next pattern goes.
            Item {
                width: 46
                height: 40
                readonly property bool cursor: strip.active && strip.cursor === backend.chain.length
                DashedBox {
                    anchors.fill: parent
                    lineWidth: parent.cursor ? 3 : 2
                    color: parent.cursor ? theme.accent : theme.foreground
                }
                Label {
                    anchors.centerIn: parent
                    text: "+"
                    font.family: theme.displayFont
                    font.pixelSize: 22
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: strip.slotPicked(backend.chain.length)
                }
            }
        }
    }

    Label {
        objectName: "songLength"
        text: qsTr("%Ln bars", "", backend.chain.length)
        color: theme.quiet
        font.pixelSize: 12
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.9
    }

    // Keeps the cursor, or the slot playing, in view.
    function reveal(index) {
        const left = index * 52, right = left + 46
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
