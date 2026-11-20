import QtQuick
import QtQuick.Controls

// A small toggle or button for the transport and the pads' settings, as
// easy to hit with a finger as with the mouse.
Rectangle {
    id: chip

    property string text
    property bool on: false
    property color onColor: theme.accent
    property color onTextColor: theme.accentForeground
    property bool bold: true

    signal clicked()
    signal pressAndHold()

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    implicitWidth: Math.max(implicitHeight, label.implicitWidth + 22)
    implicitHeight: 32
    radius: Math.min(4, theme.radius)
    opacity: enabled ? 1 : 0.4
    color: on ? onColor : tap.pressed ? alpha(theme.colors.selection, 1) : alpha(theme.colors.selection, 0.6)
    border.color: on ? onColor : theme.colors.selection

    Label {
        id: label
        anchors.centerIn: parent
        text: chip.text
        font.bold: chip.bold
        color: chip.on ? chip.onTextColor : theme.foreground
    }

    TapHandler {
        id: tap
        onTapped: chip.clicked()
        onLongPressed: chip.pressAndHold()
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
