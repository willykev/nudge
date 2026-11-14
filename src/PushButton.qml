import QtQuick

// The shell's button: an outline that fills faintly under the pointer or the
// keyboard. The one to press takes the accent on its border and its label,
// and one that destroys takes the theme's red; nothing is ever a solid block.
Rectangle {
    id: button
    property string text: ""
    property bool primary: false
    property bool danger: false
    property bool compact: false
    signal clicked()
    readonly property var ui: look.colors
    readonly property bool hot: hover.hovered || activeFocus
    readonly property color tone: danger ? ui.danger : primary ? ui.accent : ui.foreground

    implicitWidth: Math.max(compact ? 0 : 84, label.implicitWidth + (compact ? 18 : 28))
    implicitHeight: compact ? 24 : 30
    radius: theme.radius
    color: tap.pressed ? Qt.alpha(tone, 0.22) : hot ? (danger ? Qt.alpha(ui.danger, 0.16) : ui.hoverFill) : "transparent"
    border.width: look.controlBorderWidth
    border.color: danger || primary ? Qt.alpha(tone, hot ? 1 : 0.6) : hot ? Qt.alpha(tone, 0.7) : ui.controlBorder
    activeFocusOnTab: true
    Behavior on color { ColorAnimation { duration: look.reduceMotion ? 0 : 80 } }

    Keys.onReturnPressed: button.clicked()
    Keys.onEnterPressed: button.clicked()
    Keys.onSpacePressed: button.clicked()

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        textFormat: Text.PlainText
        color: button.danger || button.primary ? button.tone : button.ui.foreground
        font.family: look.fontFamily
        font.pixelSize: Math.round(look.fontSize * 0.917)
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: button.clicked() }
}
