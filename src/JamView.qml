import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Playing along with dole on the person's other machines: the ones found on
// the tailnet, and the jam once it's on.
Popup {
    id: view

    property bool rightToLeft: false

    // Sharing replaces the song here, so the window asks first.
    signal shareRequested(string address)

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 48 : 560, 560)
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
    function problemText() {
        switch (jam.problem) {
        case "noTailscale": return qsTr("Jamming finds your other machines over Tailscale, which isn't installed.")
        case "tailscaleStopped": return qsTr("Tailscale isn't connected. Connect it to find your other machines.")
        case "unreachable": return qsTr("Couldn't reach dole on %1. Is it open there, and does its firewall let in TCP port %2 on Tailscale?")
                                   .arg(jam.problemName).arg(jam.port)
        case "busy": return qsTr("That dole is in %1's jam. Join it there.").arg(jam.problemName)
        case "lost": return qsTr("Lost the connection to %1.").arg(jam.problemName)
        case "ended": return qsTr("%1 ended the jam.").arg(jam.problemName)
        }
        return ""
    }
    function names(list) {
        return list.map(m => m.name).join(", ")
    }

    onOpened: if (jam.role === "alone") jam.search()
    onClosed: jam.dismissProblem()

    ColumnLayout {
        width: parent.width
        spacing: 16
        LayoutMirroring.enabled: view.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            text: qsTr("Jam")
            font.family: theme.displayFont
            font.pixelSize: 34
            font.capitalization: Font.AllUppercase
        }
        Label {
            Layout.fillWidth: true
            visible: jam.role === "alone"
            text: qsTr("Play along with dole on your other machines, found over Tailscale: on one beat with your own song, or sharing one song.")
            color: theme.quiet
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "jamProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: view.problemText()
            color: theme.colors.red || theme.accent
            font.bold: true
            wrapMode: Text.Wrap
        }

        // Joined, or on the way.
        ColumnLayout {
            Layout.fillWidth: true
            visible: jam.role === "joined" || jam.role === "joining"
            spacing: 6
            Label {
                objectName: "jamWith"
                Layout.fillWidth: true
                text: jam.role === "joining" ? qsTr("Joining %1…").arg(jam.hostName)
                      : jam.mode === "song" ? qsTr("Sharing %1's song").arg(jam.hostName)
                                            : qsTr("Playing along with %1").arg(jam.hostName)
                font.family: theme.displayFont
                font.pixelSize: 24
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: jam.role === "joined" && jam.latency >= 0
                text: qsTr("%1 ms away").arg(view.number(jam.latency))
                      + (jam.members.length > 1 ? "  ·  " + qsTr("Also here: %1").arg(view.names(jam.members.filter(m => m.name !== jam.hostName))) : "")
                color: theme.quiet
                wrapMode: Text.Wrap
            }
            DialogButton {
                Layout.topMargin: 8
                objectName: "jamLeave"
                label: qsTr("&Leave")
                onClicked: jam.leave()
            }
        }

        // Others joined this dole.
        ColumnLayout {
            Layout.fillWidth: true
            visible: jam.role === "hosting"
            spacing: 6
            Repeater {
                model: jam.role === "hosting" ? jam.members : []
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.mode === "song" ? qsTr("%1 shares your song").arg(modelData.name)
                                                     : qsTr("%1 plays along").arg(modelData.name)
                    font.family: theme.displayFont
                    font.pixelSize: 24
                    wrapMode: Text.Wrap
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Tempo, play, and stop are the jam's: Space starts every dole on the next bar.")
                color: theme.quiet
                wrapMode: Text.Wrap
            }
            DialogButton {
                Layout.topMargin: 8
                label: qsTr("&End the jam")
                onClicked: jam.leave()
            }
        }

        // The doles to join.
        ColumnLayout {
            Layout.fillWidth: true
            visible: jam.role === "alone"
            spacing: 10

            Label {
                Layout.fillWidth: true
                visible: jam.found.length === 0 && jam.problem === ""
                text: jam.searching ? qsTr("Looking on your tailnet…")
                                    : qsTr("No other dole is open on your tailnet.")
                color: theme.quiet
                wrapMode: Text.Wrap
            }

            Repeater {
                model: jam.role === "alone" ? jam.found : []
                delegate: RowLayout {
                    required property var modelData
                    required property int index
                    objectName: "found" + index
                    Layout.fillWidth: true
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: modelData.name
                            font.family: theme.displayFont
                            font.pixelSize: 24
                            font.capitalization: Font.AllUppercase
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: (modelData.song || qsTr("Untitled"))
                                  + (modelData.playing ? "  ·  " + qsTr("playing") : "")
                                  + (modelData.role === "hosting" ? "  ·  " + qsTr("in a jam") : "")
                                  + (modelData.role === "joined" ? "  ·  " + qsTr("in %1's jam").arg(modelData.host) : "")
                            color: theme.quiet
                            elide: Text.ElideRight
                        }
                    }
                    Chip {
                        objectName: "playAlong"
                        text: qsTr("Play along")
                        onClicked: jam.join(modelData.address, "clock")
                    }
                    Chip {
                        objectName: "shareSong"
                        text: qsTr("Share song")
                        onClicked: view.shareRequested(modelData.address)
                    }
                }
            }

            DialogButton {
                Layout.topMargin: 6
                objectName: "jamSearch"
                label: qsTr("&Search again")
                enabled: !jam.searching
                onClicked: jam.search()
            }
        }
    }
}
