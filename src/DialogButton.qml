import QtQuick
import QtQuick.Controls

// A dialog button the way Omarchy's apps draw them (Omacut's DialogButton,
// MIT, David Heinemeier Hansson), in the theme's colors: the window's
// corners, capped small, and the accent on the primary one. The label marks
// its access key with &, as translations do; that letter is underlined and
// presses it, with or without Alt.
Button {
    id: control

    property bool primary: false
    property string label
    readonly property int keyAt: label.indexOf("&")
    readonly property string mnemonic: keyAt >= 0 ? label.charAt(keyAt + 1) : ""

    text: label.replace("&", "")
    focusPolicy: Qt.NoFocus
    leftPadding: 16
    rightPadding: 16
    topPadding: 8
    bottomPadding: 8

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    contentItem: Label {
        text: control.keyAt < 0 ? control.label
              : control.label.slice(0, control.keyAt) + "<u>" + control.label.charAt(control.keyAt + 1) + "</u>"
                + control.label.slice(control.keyAt + 2)
        textFormat: Text.StyledText
        color: control.primary ? theme.accentForeground : theme.foreground
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        implicitWidth: 88
        implicitHeight: 34
        radius: Math.min(4, theme.radius)
        color: control.primary ? Qt.darker(theme.accent, control.down ? 1.2 : control.hovered ? 1.08 : 1)
                               : control.alpha(theme.colors.selection, control.down ? 1 : control.hovered ? 0.85 : 0.6)
        border.width: 1
        border.color: control.primary ? Qt.darker(theme.accent, 1.15) : theme.colors.selection
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    Shortcut {
        sequences: [control.mnemonic, "Alt+" + control.mnemonic]
        enabled: control.visible && control.mnemonic !== ""
        autoRepeat: false
        onActivated: control.clicked()
    }
}
