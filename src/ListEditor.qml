import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Names and colors a list, new or existing. The colors are the theme's own,
// so a list changes color with the theme.
Popup {
    id: editor
    property var app
    property var listId: 0
    property string chosen: ""
    property string originalName: ""
    property string originalColor: ""

    function openFor(id) {
        const info = id ? backend.listInfo(id) : {}
        listId = id
        chosen = id ? info.color : backend.palette[backend.lists.length % backend.palette.length]
        originalColor = chosen
        originalName = id ? info.name : ""
        name.text = originalName
        open()
        name.forceActiveFocus()
        name.selectAll()
    }
    function save() {
        const saved = listId
            ? backend.updateList(listId, name.text === originalName ? "" : name.text,
                                 chosen === originalColor ? "" : chosen)
            : backend.addList(name.text, chosen)
        if (saved)
            close()
    }
    function step(delta) {
        const names = look.swatchNames
        const at = Math.max(0, names.indexOf(chosen))
        chosen = names[(at + delta + names.length) % names.length]
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 340
    modal: true
    focus: true
    padding: 18
    Overlay.modal: Rectangle { color: Qt.alpha(editor.app.paper, 0.7) }

    // A card with the border of the desktop's own windows.
    background: Rectangle {
        color: editor.app.ui.cardBackground
        radius: editor.app.corner
        border.width: look.controlBorderWidth
        border.color: editor.app.ui.cardBorder
    }

    contentItem: ColumnLayout {
        spacing: 12
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Text {
            text: editor.listId ? qsTr("Edit List") : qsTr("New List")
            color: editor.app.bright
            font.pixelSize: editor.app.fsTitle
            font.bold: true
        }
        InputField {
            id: name
            Layout.fillWidth: true
            placeholderText: qsTr("Name")
            onAccepted: editor.save()
            // The arrows pick the color without leaving the name.
            Keys.onUpPressed: editor.step(-1)
            Keys.onDownPressed: editor.step(1)
        }
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.leftMargin: 4
            spacing: 14
            Repeater {
                model: look.swatchNames
                Rectangle {
                    required property string modelData
                    width: 18
                    height: 18
                    radius: Math.min(editor.app.corner, 9)
                    color: editor.app.swatch(modelData)
                    // The chosen one stands out by an outline held off from it.
                    Rectangle {
                        visible: editor.chosen === parent.modelData
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: Math.min(editor.app.corner + 3, 15)
                        color: "transparent"
                        border.width: 1
                        border.color: editor.app.ink
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: editor.chosen = parent.modelData }
                }
            }
        }
        Text {
            text: qsTr("↑ ↓ change the color")
            color: editor.app.faint
            font.pixelSize: editor.app.fsSmall
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 10
            Item { Layout.fillWidth: true }
            PushButton { text: qsTr("Cancel"); onClicked: editor.close() }
            PushButton { primary: true; text: editor.listId ? qsTr("Save") : qsTr("Add"); onClicked: editor.save() }
        }
    }
}
