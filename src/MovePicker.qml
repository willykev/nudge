import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Picks the list a reminder moves to: arrows and Return, or its number.
// Drawn like the Omarchy menu.
Popup {
    id: picker
    property var app
    property var reminderId: 0
    property int current: 0

    function openFor(id) {
        reminderId = id
        const selected = backend.selected
        current = Math.max(0, backend.lists.findIndex(list => list.id === selected.listId))
        open()
        keys.forceActiveFocus()
    }
    function choose(index) {
        if (index < 0 || index >= backend.lists.length)
            return
        backend.moveToList(reminderId, backend.lists[index].id)
        close()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 300
    modal: true
    focus: true
    padding: 10
    Overlay.modal: Rectangle { color: Qt.alpha(picker.app.paper, 0.7) }

    background: Rectangle {
        color: picker.app.ui.cardBackground
        radius: picker.app.corner
        border.width: look.controlBorderWidth
        border.color: picker.app.ui.cardBorder
    }

    contentItem: ColumnLayout {
        id: keys
        spacing: 0
        focus: true
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        Keys.onUpPressed: picker.current = Math.max(0, picker.current - 1)
        Keys.onDownPressed: picker.current = Math.min(backend.lists.length - 1, picker.current + 1)
        Keys.onReturnPressed: picker.choose(picker.current)
        Keys.onEnterPressed: picker.choose(picker.current)
        Keys.onPressed: event => {
            if (event.key === Qt.Key_K) picker.current = Math.max(0, picker.current - 1)
            else if (event.key === Qt.Key_J) picker.current = Math.min(backend.lists.length - 1, picker.current + 1)
            else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) picker.choose(event.key - Qt.Key_1)
            else return
            event.accepted = true
        }

        Text {
            Layout.margins: 10
            Layout.bottomMargin: 10
            text: qsTr("Move to List")
            color: picker.app.bright
            font.pixelSize: picker.app.fsTitle
            font.bold: true
        }
        Repeater {
            model: backend.lists
            Rectangle {
                id: choice
                required property var modelData
                required property int index
                readonly property bool cursor: picker.current === index
                Layout.fillWidth: true
                implicitHeight: 28
                radius: picker.app.corner
                color: cursor ? picker.app.cursorFill : choiceHover.hovered ? picker.app.ui.controlFill : "transparent"
                border.width: cursor ? look.controlBorderWidth : 0
                border.color: picker.app.cursorBorder
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 9
                    Rectangle {
                        implicitWidth: 7
                        implicitHeight: 7
                        radius: Math.min(picker.app.corner, 3.5)
                        color: picker.app.swatch(choice.modelData.color)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: choice.modelData.name
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        color: choice.cursor ? picker.app.cursorText : picker.app.ink
                        font.pixelSize: picker.app.fsBody
                    }
                    Text {
                        visible: choice.index < 9
                        text: choice.index + 1
                        color: picker.app.faint
                        font.pixelSize: picker.app.fsSmall
                    }
                }
                HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: picker.choose(choice.index) }
            }
        }
    }
}
