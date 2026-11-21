import QtQuick
import QtQuick.Controls.Basic

// A text field drawn the way the Omarchy shell draws its own: a faint fill
// and a hairline border in the theme's foreground, which lift on hover and
// focus. Given a `save` function it shows a stored value and saves what was
// typed when it is left or Return is pressed.
TextField {
    id: field
    property string value: ""
    property var save: null
    property string storedValue: value
    property string initialText: ""
    property var editSource: null
    property var original: null
    property bool edited: false
    readonly property var ui: look.colors

    font.family: look.fontFamily
    font.pixelSize: look.fontSize
    color: ui.foreground
    selectionColor: ui.selectionFill
    selectedTextColor: ui.foreground
    placeholderTextColor: ui.faint
    leftPadding: 10
    rightPadding: 10
    topPadding: 7
    bottomPadding: 7
    selectByMouse: true

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 30
        radius: theme.radius
        color: field.activeFocus ? field.ui.focusFill : field.hovered ? field.ui.hoverFill : field.ui.controlFill
        border.width: look.controlBorderWidth
        border.color: field.activeFocus ? field.ui.focusBorder : field.hovered ? field.ui.hoverBorder : field.ui.controlBorder
        Behavior on color { ColorAnimation { duration: look.reduceMotion ? 0 : 80 } }
    }

    // A long value shows from its start, not from wherever the cursor was.
    function showValue() {
        if (!save)
            return
        edited = false
        original = null
        initialText = value
        text = value
        if (!activeFocus)
            cursorPosition = 0
    }
    function flush() {
        if (save && edited) {
            const source = original
            edited = false
            save(text, source)
            showValue()
        }
    }
    onTextEdited: {
        if (!save)
            return
        if (!edited)
            original = editSource
        edited = text !== initialText
        if (!edited)
            showValue()
    }
    onStoredValueChanged: showValue()
    onValueChanged: if (!edited) showValue()
    Component.onCompleted: showValue()
    onEditingFinished: {
        flush()
        // What was typed becomes what was understood: "tomorrow" turns into
        // the date, and something unreadable goes back to what it was.
        showValue()
    }
}
