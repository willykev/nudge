import QtQuick
import QtQuick.Shapes

// Line icons on a 24-unit grid, drawn the way Hype's AppIcon does. The link
// is Feather's (MIT); the rest are Nudge's own.
Item {
    id: glyph
    property string name
    property color color: look.colors.foreground
    property real size: 16
    property real weight: 1.8
    property bool filled: false

    implicitWidth: size
    implicitHeight: size

    readonly property var paths: ({
        "today": "M12 8A4 4 0 1 0 12 16A4 4 0 1 0 12 8Z M12 3V5 M12 19V21 M3 12H5 M19 12H21 M5.6 5.6L7 7 M17 17L18.4 18.4 M5.6 18.4L7 17 M17 7L18.4 5.6",
        "calendar": "M4 6H20V20H4Z M4 10H20 M8 3V7 M16 3V7",
        "clock": "M12 3A9 9 0 1 0 12 21A9 9 0 1 0 12 3Z M12 7.5V12L15.5 14",
        "tray": "M4 13L6.5 5H17.5L20 13V19H4Z M4 13H9A3 3 0 0 0 15 13H20",
        "flag": "M6 21V4H18L15.5 8.5L18 13H6",
        "check": "M5 12.5L10 17.5L19 7",
        "list": "M9 6H20 M9 12H20 M9 18H20 M4.5 6H4.51 M4.5 12H4.51 M4.5 18H4.51",
        "tag": "M9.5 4L7.5 20 M16.5 4L14.5 20 M4.5 9H20 M3.5 15H19",
        "plus": "M12 5V19 M5 12H19",
        "minus": "M5 12H19",
        "search": "M10.5 4A6.5 6.5 0 1 0 10.5 17A6.5 6.5 0 1 0 10.5 4Z M15.5 15.5L20 20",
        "repeat": "M4 11V9A3 3 0 0 1 7 6H19 M16 3L19 6L16 9 M20 13V15A3 3 0 0 1 17 18H5 M8 21L5 18L8 15",
        "chevron-right": "M9 6L15 12L9 18",
        "chevron-down": "M6 9L12 15L18 9",
        "chevron-left": "M15 6L9 12L15 18",
        "link": "M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71 M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71",
        "info": "M12 3A9 9 0 1 0 12 21A9 9 0 1 0 12 3Z M12 11V16.5 M12 7.5H12.01",
        "eye": "M2 12C5 6.5 8.5 5 12 5C15.5 5 19 6.5 22 12C19 17.5 15.5 19 12 19C8.5 19 5 17.5 2 12Z M12 9A3 3 0 1 0 12 15A3 3 0 1 0 12 9Z",
        "sort": "M4 7H20 M7 12H17 M10 17H14",
        "dots": "M5 12H5.01 M12 12H12.01 M19 12H19.01",
        "trash": "M5 7H19 M10 4H14 M7 7L8 20H16L17 7 M10 11V16 M14 11V16",
        "close": "M6 6L18 18 M18 6L6 18",
        "bell": "M6 16V11A6 6 0 0 1 18 11V16L20 18H4Z M10 21H14",
        "subtask": "M6 5V13H17 M13 9L17 13L13 17",
        "pencil": "M4 20L5 16L16 5L19 8L8 19Z M14 7L17 10"
    })

    Shape {
        width: 24
        height: 24
        anchors.centerIn: parent
        scale: glyph.size / 24
        antialiasing: true
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: glyph.filled ? glyph.color : "transparent"
            strokeColor: glyph.color
            strokeWidth: glyph.weight
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: glyph.paths[glyph.name] || "" }
        }
    }
}
