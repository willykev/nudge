import QtQuick

// On or off, as the shell's own switch: one color, a knob that brightens and
// crosses over, and square when the desktop's windows are.
Item {
    id: toggle
    property bool checked: false
    property string text: ""
    signal toggled(bool checked)
    readonly property var ui: look.colors
    readonly property bool rounded: theme.radius > 0
    readonly property bool mirrored: LayoutMirroring.enabled

    implicitWidth: track.width + (text !== "" ? label.implicitWidth + 9 : 0)
    implicitHeight: 22
    activeFocusOnTab: true

    function flip() { toggled(!checked) }
    Keys.onSpacePressed: flip()
    Keys.onReturnPressed: flip()
    Keys.onEnterPressed: flip()

    Rectangle {
        id: track
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 38
        height: 20
        radius: toggle.rounded ? height / 2 : 0
        color: toggle.checked ? toggle.ui.selectedFill : hover.hovered || toggle.activeFocus ? toggle.ui.hoverFill : toggle.ui.controlFill
        border.width: look.controlBorderWidth
        border.color: toggle.activeFocus ? toggle.ui.foreground : hover.hovered ? toggle.ui.hoverBorder : toggle.ui.controlBorder
        Behavior on color { ColorAnimation { duration: look.reduceMotion ? 0 : 120 } }

        Rectangle {
            width: 12
            height: 12
            radius: toggle.rounded ? 6 : 0
            anchors.verticalCenter: parent.verticalCenter
            x: toggle.checked !== toggle.mirrored ? parent.width - width - 4 : 4
            color: toggle.checked ? toggle.ui.bright : toggle.ui.faint
            Behavior on x { NumberAnimation { duration: look.reduceMotion ? 0 : 120; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: look.reduceMotion ? 0 : 120 } }
        }
    }
    Text {
        id: label
        visible: toggle.text !== ""
        anchors.left: track.right
        anchors.leftMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        text: toggle.text
        textFormat: Text.PlainText
        color: toggle.checked ? toggle.ui.foreground : toggle.ui.dim
        font.family: look.fontFamily
        font.pixelSize: look.fontSize
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: toggle.flip() }
}
