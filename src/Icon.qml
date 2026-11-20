import QtQuick
import QtQuick.Shapes

// The few marks dole draws instead of borrowing glyphs from a font: the
// transport's play, stop, and record, the disclosure triangles, a cross,
// and arrows early and late. One solid fill each, in the text's colour.
Item {
    id: icon

    property string name: "play"
    property color color: theme.foreground

    implicitWidth: 14
    implicitHeight: 14

    // A closed track points the way its row reads.
    transform: Scale {
        origin.x: icon.width / 2
        xScale: icon.name === "closed" && icon.LayoutMirroring.enabled ? -1 : 1
    }

    readonly property real w: width
    readonly property real h: height

    Rectangle {
        visible: icon.name === "stop"
        anchors.fill: parent
        color: icon.color
    }
    Rectangle {
        visible: icon.name === "record"
        anchors.fill: parent
        radius: width / 2
        color: icon.color
    }
    // The cross: two bars across each other.
    Repeater {
        model: icon.name === "close" ? [45, -45] : []
        Rectangle {
            required property int modelData
            anchors.centerIn: parent
            width: icon.w * 1.2
            height: Math.max(2, icon.h * 0.16)
            rotation: modelData
            color: icon.color
        }
    }

    Shape {
        anchors.fill: parent
        visible: ["play", "open", "closed", "left", "right"].indexOf(icon.name) >= 0
        antialiasing: true
        layer.enabled: true
        layer.samples: 4
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: icon.color
            PathPolyline {
                path: {
                    const w = icon.w, h = icon.h
                    switch (icon.name) {
                    case "play": return [Qt.point(0, 0), Qt.point(w, h / 2), Qt.point(0, h), Qt.point(0, 0)]
                    case "closed": return [Qt.point(w * 0.2, 0), Qt.point(w * 0.9, h / 2), Qt.point(w * 0.2, h), Qt.point(w * 0.2, 0)]
                    case "open": return [Qt.point(0, h * 0.2), Qt.point(w, h * 0.2), Qt.point(w / 2, h * 0.9), Qt.point(0, h * 0.2)]
                    // An arrow: a head and a shaft.
                    case "left": return [Qt.point(0, h / 2), Qt.point(w * 0.5, 0), Qt.point(w * 0.5, h * 0.35), Qt.point(w, h * 0.35),
                                         Qt.point(w, h * 0.65), Qt.point(w * 0.5, h * 0.65), Qt.point(w * 0.5, h), Qt.point(0, h / 2)]
                    case "right": return [Qt.point(w, h / 2), Qt.point(w * 0.5, 0), Qt.point(w * 0.5, h * 0.35), Qt.point(0, h * 0.35),
                                          Qt.point(0, h * 0.65), Qt.point(w * 0.5, h * 0.65), Qt.point(w * 0.5, h), Qt.point(w, h / 2)]
                    }
                    return []
                }
            }
        }
    }
}
