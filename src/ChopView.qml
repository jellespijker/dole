import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Chopping a sample onto pads: its waveform cut into slices, each heard on
// a tap, then laid out on the pads from the one chosen on.
Popup {
    id: chop

    property bool rightToLeft: false
    property int selected: 0
    readonly property var slices: backend.chopSlices
    readonly property int count: slices.length
    readonly property int firstPad: backend.selectedPad
    readonly property int fits: Math.min(count, 16 - firstPad)

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 64 : 900, 980)
    modal: true
    focus: true
    padding: 24
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: theme.background
        border.color: theme.colors.selection
        radius: theme.radius
    }

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function pick(index) {
        if (count === 0)
            return
        selected = (index + count) % count
        backend.previewSlice(selected)
    }
    function assign() {
        if (backend.assignChops(firstPad) > 0)
            close()
    }

    onOpened: {
        selected = 0
        if (backend.chopState === "empty")
            backend.chooseChop()
    }
    onSlicesChanged: if (selected >= count) selected = 0
    onSelectedChanged: wave.requestPaint()

    ColumnLayout {
        anchors.fill: parent
        spacing: 16
        focus: true
        // A popup is not an Item and doesn't inherit the window's mirroring,
        // so its content opts in.
        LayoutMirroring.enabled: chop.rightToLeft
        LayoutMirroring.childrenInherit: true

        Keys.onLeftPressed: chop.pick(chop.selected - 1)
        Keys.onRightPressed: chop.pick(chop.selected + 1)
        Keys.onSpacePressed: chop.pick(chop.selected)
        Keys.onReturnPressed: chop.assign()
        Keys.onEnterPressed: chop.assign()

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                text: qsTr("Chop")
                font.bold: true
                font.pixelSize: 18
            }
            Label {
                objectName: "chopName"
                Layout.fillWidth: true
                text: backend.chopState === "loading" ? qsTr("Loading %1…").arg(backend.chopName)
                      : backend.chopState === "failed" ? qsTr("Can't load %1").arg(backend.chopName)
                      : backend.chopName
                color: backend.chopState === "failed" ? theme.colors.red : theme.colors.muted
                elide: Text.ElideMiddle
            }
            DialogButton {
                label: qsTr("&Load…")
                onClicked: backend.chooseChop()
            }
        }

        // The waveform, with the slices shaded in turn and the chosen one lit.
        // Time runs left to right in every language.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: true

            Rectangle {
                anchors.fill: parent
                radius: Math.min(4, theme.radius)
                color: chop.alpha(theme.colors.selection, 0.35)
            }

            Canvas {
                id: wave
                objectName: "wave"
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const peaks = backend.chopPeaks
                    const slices = chop.slices
                    for (let i = 0; i < slices.length; ++i) {
                        const x0 = slices[i] * width
                        const x1 = (i + 1 < slices.length ? slices[i + 1] : 1) * width
                        ctx.fillStyle = i === chop.selected ? chop.alpha(theme.accent, 0.3)
                                      : chop.alpha(theme.colors.selection, i % 2 === 0 ? 0.6 : 0.2)
                        ctx.fillRect(x0, 0, x1 - x0, height)
                        ctx.fillStyle = theme.accent
                        ctx.fillRect(x0, 0, 2, height)
                    }
                    ctx.fillStyle = theme.foreground
                    const mid = height / 2
                    for (let p = 0; p < peaks.length; ++p) {
                        const x = p * width / peaks.length
                        const h = Math.max(1, peaks[p] * (height / 2 - 8))
                        ctx.fillRect(x, mid - h, Math.max(1, width / peaks.length - 0.5), 2 * h)
                    }
                }
                Connections {
                    target: backend
                    function onChopChanged() { wave.requestPaint() }
                }
                Connections {
                    target: theme
                    function onChanged() { wave.requestPaint() }
                }
                onWidthChanged: requestPaint()
            }

            // Each chop's number over it.
            Repeater {
                model: chop.slices
                delegate: Label {
                    required property int index
                    required property real modelData
                    x: modelData * wave.width + 6
                    y: 4
                    text: (index + 1).toLocaleString(Qt.locale(translator.language), 'f', 0)
                    font.bold: true
                    font.pixelSize: 11
                    color: index === chop.selected ? theme.accent : theme.colors.muted
                }
            }

            Label {
                anchors.centerIn: parent
                visible: backend.chopState === "empty"
                text: qsTr("Load a loop to chop: a break, a soul record, a film.")
                color: theme.colors.muted
            }

            TapHandler {
                onTapped: (point) => {
                    const at = point.position.x / wave.width
                    let index = 0
                    for (let i = 0; i < chop.count; ++i)
                        if (chop.slices[i] <= at)
                            index = i
                    chop.pick(index)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("Slice")
                color: theme.colors.muted
            }
            DialogButton { label: "4"; onClicked: backend.sliceEven(4) }
            DialogButton { label: "8"; onClicked: backend.sliceEven(8) }
            DialogButton { label: "16"; onClicked: backend.sliceEven(16) }
            DialogButton {
                objectName: "hitsButton"
                label: qsTr("At the &hits")
                onClicked: backend.sliceTransients()
            }
            Item { Layout.fillWidth: true }
            Label {
                objectName: "chopCount"
                text: qsTr("%Ln chops", "", chop.count)
                color: theme.colors.muted
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: chop.fits > 0
                      ? qsTr("Onto pads %1 to %2. Pick another pad first to start elsewhere.")
                          .arg((chop.firstPad + 1).toLocaleString(Qt.locale(translator.language), 'f', 0))
                          .arg((chop.firstPad + chop.fits).toLocaleString(Qt.locale(translator.language), 'f', 0))
                      : ""
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            DialogButton {
                label: qsTr("&Cancel")
                onClicked: chop.close()
            }
            DialogButton {
                objectName: "assignButton"
                label: qsTr("&Assign to pads")
                primary: true
                enabled: chop.fits > 0
                onClicked: chop.assign()
            }
        }
    }
}
