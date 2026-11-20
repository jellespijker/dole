import QtQuick
import QtQuick.Shapes

// A dashed outline: what is there to be filled, an empty pad or pattern, or
// the place the next one goes.
Shape {
    id: box

    property color color: theme.foreground
    property real lineWidth: 2

    // Drawn through a layer, which respects the clip of a scrolled list; a
    // bare shape is drawn past it.
    layer.enabled: true
    layer.samples: 4



    ShapePath {
        strokeColor: box.color
        strokeWidth: box.lineWidth
        strokeStyle: ShapePath.DashLine
        dashPattern: [3, 2]
        fillColor: "transparent"
        joinStyle: ShapePath.MiterJoin
        capStyle: ShapePath.FlatCap
        startX: box.lineWidth / 2
        startY: box.lineWidth / 2
        PathLine { x: box.width - box.lineWidth / 2; y: box.lineWidth / 2 }
        PathLine { x: box.width - box.lineWidth / 2; y: box.height - box.lineWidth / 2 }
        PathLine { x: box.lineWidth / 2; y: box.height - box.lineWidth / 2 }
        PathLine { x: box.lineWidth / 2; y: box.lineWidth / 2 }
    }
}
