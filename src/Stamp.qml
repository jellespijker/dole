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

    // A stamp put down while dole is open lands with a thud: it comes down
    // from a little above the sheet. Ones already there when it opens just are.
    property bool ready: false
    Component.onCompleted: ready = true
    onVisibleChanged: if (visible && ready) land.restart()
    ParallelAnimation {
        id: land
        NumberAnimation { target: stamp; property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.OutCubic }
        NumberAnimation {
            target: stamp
            property: "scale"
            from: theme.motion ? 1.5 : 1
            to: 1
            duration: 160
            easing.type: Easing.OutCubic
        }
    }

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
