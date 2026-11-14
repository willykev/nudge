import QtQuick
import QtQuick.Controls.Basic

// A choice among a few, in the shell's control style, opening onto a list
// drawn like the Omarchy menu.
ComboBox {
    id: box
    readonly property var ui: look.colors
    // Clicked open, it lets go of the keyboard once its list closes, so the
    // keys meant for elsewhere do not change it unseen.
    signal letGo()

    font.family: look.fontFamily
    font.pixelSize: look.fontSize
    implicitHeight: 30
    leftPadding: 10
    rightPadding: 28

    // Closed, the keys that would change the choice unseen (the arrows, Home
    // and End, and letters, which pick by name) open the list instead, where
    // the choice shows before Return makes it.
    Keys.onPressed: event => {
        const letter = event.text !== "" && event.text.charCodeAt(0) > 32 && event.text.charCodeAt(0) !== 127
        if (!popup.visible && (letter || [Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].includes(event.key)))
            popup.open()
    }

    contentItem: Text {
        text: box.displayText
        textFormat: Text.PlainText
        color: box.ui.foreground
        font: box.font
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignLeft
        elide: Text.ElideRight
    }
    indicator: Glyph {
        x: box.mirrored ? 8 : box.width - width - 8
        y: (box.height - height) / 2
        name: "chevron-down"
        size: 12
        color: box.ui.dim
    }
    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 30
        radius: theme.radius
        color: box.activeFocus || box.popup.visible ? box.ui.focusFill : box.hovered ? box.ui.hoverFill : box.ui.controlFill
        border.width: look.controlBorderWidth
        border.color: box.activeFocus || box.popup.visible ? box.ui.focusBorder : box.hovered ? box.ui.hoverBorder : box.ui.controlBorder
        Behavior on color { ColorAnimation { duration: look.reduceMotion ? 0 : 80 } }
    }

    delegate: ItemDelegate {
        id: option
        required property int index
        width: ListView.view ? ListView.view.width : box.width
        height: 28
        padding: 0
        highlighted: box.highlightedIndex === index

        contentItem: Text {
            leftPadding: 10
            rightPadding: 10
            text: box.textAt(option.index)
            textFormat: Text.PlainText
            color: option.highlighted ? box.ui.menuSelectedText : box.ui.menuText
            font: box.font
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignLeft
            elide: Text.ElideRight
        }
        background: Rectangle { color: option.highlighted ? box.ui.menuSelectedFill : "transparent" }
    }

    popup: Popup {
        y: box.height + 2
        width: box.width
        padding: 1
        topPadding: 4
        bottomPadding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 320)
        onClosed: if (box.activeFocus && box.focusReason === Qt.MouseFocusReason) box.letGo()

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: box.popup.visible ? box.delegateModel : null
            currentIndex: box.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }
        background: Rectangle {
            color: box.ui.menuBackground
            radius: theme.radius
            border.width: 1
            border.color: box.ui.menuBorder
        }
    }
}
