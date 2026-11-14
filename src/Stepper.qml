import QtQuick
import QtQuick.Controls.Basic

// A number with a step either way, in the shell's control style.
SpinBox {
    id: stepper
    readonly property var ui: look.colors

    editable: true
    font.family: look.fontFamily
    font.pixelSize: look.fontSize
    implicitWidth: 104
    implicitHeight: 30
    leftPadding: 28
    rightPadding: 28

    contentItem: TextInput {
        text: stepper.textFromValue(stepper.value, stepper.locale)
        font: stepper.font
        color: stepper.ui.foreground
        selectionColor: stepper.ui.selectionFill
        selectedTextColor: stepper.ui.foreground
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !stepper.editable
        validator: stepper.validator
        inputMethodHints: Qt.ImhDigitsOnly
        selectByMouse: true
    }
    up.indicator: Rectangle {
        x: stepper.mirrored ? 1 : stepper.width - width - 1
        y: 1
        width: 26
        height: stepper.height - 2
        color: stepper.up.pressed ? stepper.ui.pressedFill : stepper.up.hovered ? stepper.ui.hoverFill : "transparent"
        Glyph { anchors.centerIn: parent; name: "plus"; size: 12; color: stepper.ui.dim }
    }
    down.indicator: Rectangle {
        x: stepper.mirrored ? stepper.width - width - 1 : 1
        y: 1
        width: 26
        height: stepper.height - 2
        color: stepper.down.pressed ? stepper.ui.pressedFill : stepper.down.hovered ? stepper.ui.hoverFill : "transparent"
        Glyph { anchors.centerIn: parent; name: "minus"; size: 12; color: stepper.ui.dim }
    }
    background: Rectangle {
        implicitWidth: 104
        implicitHeight: 30
        radius: theme.radius
        color: stepper.activeFocus ? stepper.ui.focusFill : stepper.hovered ? stepper.ui.hoverFill : stepper.ui.controlFill
        border.width: look.controlBorderWidth
        border.color: stepper.activeFocus ? stepper.ui.focusBorder : stepper.hovered ? stepper.ui.hoverBorder : stepper.ui.controlBorder
        Behavior on color { ColorAnimation { duration: look.reduceMotion ? 0 : 80 } }
    }
}
