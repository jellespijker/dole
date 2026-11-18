import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asks before unsaved work is lost. Escape, or closing it, cancels: the
// answer that loses nothing.
Popup {
    id: dialog

    property string text
    property bool rightToLeft: false
    // What to do once the song is saved or let go.
    property var action: null
    // Save was chosen, and the action waits for it to finish.
    property bool waiting: false

    signal saveChosen()
    signal discardChosen()

    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 24
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: theme.background
        border.color: theme.colors.selection
        radius: theme.radius
    }

    function choose(save) {
        close()
        if (save)
            saveChosen()
        else
            discardChosen()
    }

    // A popup is not an Item and doesn't inherit the window's mirroring, so
    // its content opts in.
    ColumnLayout {
        spacing: 20
        LayoutMirroring.enabled: dialog.rightToLeft
        LayoutMirroring.childrenInherit: true
        focus: true

        Keys.onReturnPressed: dialog.choose(true)
        Keys.onEnterPressed: dialog.choose(true)

        Label {
            objectName: "confirmText"
            Layout.maximumWidth: 420
            text: dialog.text
            wrapMode: Text.Wrap
            font.pixelSize: 15
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8

            DialogButton {
                objectName: "cancelButton"
                label: qsTr("&Cancel")
                onClicked: dialog.close()
            }
            DialogButton {
                objectName: "discardButton"
                label: qsTr("&Don't Save")
                onClicked: dialog.choose(false)
            }
            DialogButton {
                objectName: "saveButton"
                label: qsTr("&Save")
                primary: true
                onClicked: dialog.choose(true)
            }
        }
    }
}
