import QtQuick
import QtQuick.Controls

// A word stamped across something at a slant, as on a flyer: a track is
// muted by a stamp over it, never by fading it out.
Rectangle {
    id: stamp

    property alias text: label.text
    property color ink: theme.colors.red
    property int size: 26

    implicitWidth: label.implicitWidth + 18
    implicitHeight: label.implicitHeight + 2
    rotation: -4
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.7)
    border.width: 3
    border.color: ink

    Label {
        id: label
        anchors.centerIn: parent
        font.family: theme.displayFont
        font.pixelSize: stamp.size
        font.letterSpacing: stamp.size * 0.08
        font.capitalization: Font.AllUppercase
        color: stamp.ink
    }
}
