import QtQuick
import QtQuick.Controls

// A button or toggle, printed: an outline in the ink it sits on, filled solid
// when it is on. As easy to hit with a finger as with the mouse.
Rectangle {
    id: chip

    property string text
    property bool on: false
    // The ink it is printed in, and what fills it when it is on.
    property color ink: theme.foreground
    property color onColor: theme.accent
    property color onTextColor: theme.accentForeground
    property bool bold: true
    // Set in the display face, for the transport.
    property bool display: false
    property string icon: ""

    // Held down, for buttons that repeat while held.
    readonly property bool pressed: tap.pressed

    signal clicked()
    signal pressAndHold()

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    implicitWidth: Math.max(implicitHeight, content.implicitWidth + (display ? 36 : 24))
    implicitHeight: 34
    radius: Math.min(4, theme.radius)
    opacity: enabled ? 1 : 0.4
    color: on ? onColor : tap.pressed ? alpha(ink, 0.2) : "transparent"
    border.width: 2
    border.color: on ? onColor : ink

    Row {
        id: content
        anchors.centerIn: parent
        spacing: chip.display ? 10 : 7
        Icon {
            visible: chip.icon !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: chip.display ? 16 : 11
            height: width
            name: chip.icon
            color: label.color
        }
        Label {
            id: label
            visible: chip.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            font.family: chip.display ? theme.displayFont : theme.typeFont
            font.pixelSize: chip.display ? 24 : 13
            font.bold: chip.bold && !chip.display
            font.capitalization: Font.AllUppercase
            font.letterSpacing: chip.display ? 0.5 : 0.4
            color: chip.on ? chip.onTextColor : chip.ink
        }
    }

    TapHandler {
        id: tap
        onTapped: chip.clicked()
        onLongPressed: chip.pressAndHold()
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
