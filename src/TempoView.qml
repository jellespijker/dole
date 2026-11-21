import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The tempo and swing, sized for fingers: steps that repeat while held,
// and a pad to tap the tempo in on.
Popup {
    id: view

    property bool rightToLeft: false
    property var taps: []

    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 24
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: theme.background
        border.width: 3
        border.color: theme.foreground
        radius: theme.radius
    }

    function number(n) { return n.toLocaleString(Qt.locale(translator.language), 'f', 0) }
    // The tempo from the last few taps; a pause of two seconds starts over.
    function tap() {
        const now = Date.now()
        if (taps.length > 0 && now - taps[taps.length - 1] > 2000)
            taps = []
        taps = taps.concat([now]).slice(-5)
        tapFlash.restart()
        if (taps.length < 2)
            return
        const beat = (taps[taps.length - 1] - taps[0]) / (taps.length - 1)
        backend.setBpm(Math.round(60000 / beat))
    }

    onOpened: taps = []

    // A step held down repeats, slowly at first.
    component Step: Chip {
        id: step
        property int delta
        property var change
        display: true
        implicitWidth: 64
        implicitHeight: 56
        text: (delta > 0 ? "+" : "−") + view.number(Math.abs(delta))
        onClicked: change(delta)
        Timer {
            running: step.pressed
            interval: repeating ? 70 : 450
            repeat: true
            property bool repeating: false
            onRunningChanged: repeating = false
            onTriggered: {
                repeating = true
                step.change(step.delta)
            }
        }
    }

    ColumnLayout {
        spacing: 18
        LayoutMirroring.enabled: view.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            text: qsTr("Tempo")
            font.family: theme.displayFont
            font.pixelSize: 34
            font.capitalization: Font.AllUppercase
        }

        // Slower is left in every language, as on a dial.
        RowLayout {
            spacing: 8
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: true
            Step { delta: -5; change: backend.changeBpm }
            Step { delta: -1; change: backend.changeBpm }
            Label {
                objectName: "tempoValue"
                Layout.minimumWidth: 140
                horizontalAlignment: Text.AlignHCenter
                text: view.number(backend.bpm)
                font.family: theme.displayFont
                font.pixelSize: 72
            }
            Step { delta: 1; change: backend.changeBpm }
            Step { delta: 5; change: backend.changeBpm }
        }

        // Tapped on the press, not the release, so it keeps the time the
        // finger keeps.
        Rectangle {
            objectName: "tapTempo"
            Layout.fillWidth: true
            implicitHeight: 84
            color: tapFlash.running ? theme.foreground : "transparent"
            border.width: 3
            border.color: theme.foreground
            radius: Math.min(4, theme.radius)
            Label {
                anchors.centerIn: parent
                text: view.taps.length > 0 && view.taps.length < 2 ? qsTr("Again…") : qsTr("Tap in time")
                color: tapFlash.running ? theme.background : theme.foreground
                font.family: theme.displayFont
                font.pixelSize: 30
                font.capitalization: Font.AllUppercase
            }
            Timer { id: tapFlash; interval: 90 }
            MouseArea {
                anchors.fill: parent
                onPressed: view.tap()
            }
        }

        RowLayout {
            spacing: 8
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Swing")
                font.family: theme.displayFont
                font.pixelSize: 28
                font.capitalization: Font.AllUppercase
            }
            Step { delta: -1; change: backend.changeSwing }
            Label {
                objectName: "swingValue"
                Layout.minimumWidth: 90
                horizontalAlignment: Text.AlignHCenter
                text: view.number(backend.swing) + "%"
                font.family: theme.displayFont
                font.pixelSize: 40
            }
            Step { delta: 1; change: backend.changeSwing }
        }
    }
}
