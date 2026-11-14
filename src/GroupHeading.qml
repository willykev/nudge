import QtQuick

// A heading between groups of reminders: a day, a list, or one of a list's
// own sections, which can be renamed and removed from here. Small and bold
// with a rule running on from it, like a section in the shell's panels.
Item {
    id: group

    required property int index
    required property string heading
    required property string tint
    required property var sectionId
    property var app

    width: ListView.view ? ListView.view.width : 0
    height: index === 0 ? 26 : 40
    ListView.onReused: opacity = 1

    HoverHandler { id: hover }
    TapHandler { onDoubleTapped: if (group.sectionId > 0) app.renameSection(group.sectionId) }

    Text {
        id: label
        anchors { left: parent.left; bottom: parent.bottom }
        anchors.leftMargin: app.gutter
        anchors.bottomMargin: 7
        width: Math.min(implicitWidth, parent.width - app.gutter - 90)
        text: group.heading
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignLeft
        elide: Text.ElideRight
        color: app.dim
        font.pixelSize: app.fsSmall
        font.bold: true
    }
    Rectangle {
        anchors { left: label.right; right: tools.visible ? tools.left : parent.right; verticalCenter: label.verticalCenter }
        anchors.leftMargin: 10
        anchors.rightMargin: tools.visible ? 8 : 24
        height: 1
        color: app.rule
    }

    Row {
        id: tools
        visible: group.sectionId > 0 && hover.hovered
        anchors { right: parent.right; verticalCenter: label.verticalCenter }
        anchors.rightMargin: 22
        spacing: 2

        Repeater {
            model: [
                { icon: "pencil", action: () => app.renameSection(group.sectionId) },
                { icon: "trash", action: () => backend.removeSection(group.sectionId) }
            ]
            Rectangle {
                required property var modelData
                width: 22
                height: 22
                radius: app.corner
                color: toolHover.hovered ? app.cursorFill : "transparent"
                Glyph { anchors.centerIn: parent; name: modelData.icon; size: 12; color: app.dim }
                HoverHandler { id: toolHover; cursorShape: Qt.PointingHandCursor }
                // A title being typed is finished first, so its new reminder
                // is added and named in one step, before this one.
                TapHandler { onTapped: { app.focusList(); modelData.action() } }
            }
        }
    }
}
