import QtQuick
import QtQuick.Layouts

// One reminder in the list: the box that completes it, its title, and
// whatever else it carries. The title turns into a text field while it is
// being typed.
Item {
    id: row

    required property int index
    required property var rid
    required property string title
    required property string notes
    required property bool done
    required property bool flagged
    required property int priority
    required property string dueText
    required property bool overdue
    required property string repeatText
    required property string tags
    required property string link
    required property int depth
    required property int childCount
    required property bool collapsed
    required property string listName
    required property string tint
    required property string parentTitle
    property var app

    readonly property bool selected: backend.selectedId === rid
    // The cursor: this row is the one the keys act on.
    readonly property bool cursor: selected && app.pane === "list"
    readonly property bool editing: app.editingId === rid
    readonly property bool draft: backend.draftId === rid
    readonly property bool hasMeta: dueText !== "" || repeatText !== "" || tags !== "" || link !== ""
                                    || listName !== "" || parentTitle !== ""
    readonly property int inset: app.gutter + depth * 24

    width: ListView.view ? ListView.view.width : 0
    height: content.implicitHeight + 12 + nudge
    // A row that grows while the rows below are still sliding into place (a
    // date read out of a title typed fast) is not made room for: the list
    // leaves rows in mid-slide alone, and nothing tells it to look again.
    // So once they have settled, the row changes size by nothing at all,
    // which does.
    property int nudge: 0
    property bool settling: false
    onHeightChanged: if (!settling) settle.restart()
    Timer {
        id: settle
        interval: 240
        onTriggered: {
            row.settling = true
            row.nudge = 1
            row.nudge = 0
            row.settling = false
        }
    }

    // A row that left mid-fade must not come back from the pool half faded.
    ListView.onReused: opacity = 1

    function startEditing() {
        app.activeEditor = editor
        editor.text = title
        editor.forceActiveFocus()
        editor.cursorPosition = editor.text.length
    }
    onEditingChanged: {
        if (editing)
            startEditing()
        else
            editor.focus = false // or the list would hand the keyboard straight back
    }
    Component.onCompleted: if (editing) startEditing()
    // A row taken away mid-edit keeps what was typed, as clicking away does:
    // the view moved, or the language changed, while another window had the
    // keyboard. One scrolled out of sight while its field has the keyboard
    // is still being typed in, and comes back with the typing.
    Component.onDestruction: if (row.editing) app.finishEdit(editor.text, false)
    ListView.onPooled: if (row.editing && !editor.activeFocus) app.finishEdit(editor.text, false)

    // The row under the keys is drawn as the Omarchy menu draws its own: a
    // faint fill, a hairline around it, and its title in the accent.
    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        radius: app.corner
        color: row.selected ? app.cursorFill : hover.hovered ? app.ui.controlFill : "transparent"
        border.width: row.cursor ? look.controlBorderWidth : 0
        border.color: app.cursorBorder
        Behavior on color { ColorAnimation { duration: app.ms(60) } }
    }

    HoverHandler { id: hover }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: app.select(row.rid)
        onDoubleTapped: app.editTitle()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: { app.select(row.rid); app.openRowMenu() }
    }

    // A subtask hangs from a line dropped from its parent's box.
    Rectangle {
        visible: row.depth > 0
        anchors.left: parent.left
        anchors.leftMargin: app.gutter + 7
        width: 1
        height: parent.height
        color: app.rule
    }

    // Folds a reminder's subtasks away.
    Item {
        visible: row.childCount > 0
        // Anchored, not placed by x, so it mirrors with the rest.
        anchors.left: parent.left
        anchors.leftMargin: row.inset - 16
        y: 5
        width: 14
        height: 20
        Glyph {
            anchors.centerIn: parent
            name: row.collapsed ? "chevron-right" : "chevron-down"
            size: 10
            color: app.dim
        }
        TapHandler { onTapped: backend.toggleCollapsed(row.rid) }
    }

    RowLayout {
        id: content
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
        anchors.leftMargin: row.inset
        anchors.rightMargin: 24
        spacing: 10

        // A box with the desktop's corners: square where windows are square,
        // round where they are round. One color, like the shell's controls.
        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 1
            implicitWidth: 15
            implicitHeight: 15
            radius: Math.min(app.corner, 7.5)
            color: row.done ? app.chosen : boxHover.hovered ? app.cursorFill : "transparent"
            border.width: 1
            border.color: boxHover.hovered ? app.ink : row.done ? app.cursorBorder : app.ui.controlBorder
            Behavior on color { ColorAnimation { duration: app.ms(100) } }

            Glyph {
                visible: row.done
                anchors.centerIn: parent
                name: "check"
                size: 11
                weight: 2.6
                color: app.dim
            }
            HoverHandler { id: boxHover; cursorShape: Qt.PointingHandCursor }
            // A click here is a click on the row first, so a title being typed
            // is finished, and a new reminder settled, before the completion
            // is a step of its own.
            TapHandler { onTapped: { app.select(row.rid); backend.toggleCompleted(row.rid) } }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: 7

                Text {
                    visible: row.priority > 0
                    Layout.alignment: Qt.AlignTop
                    text: "!".repeat(row.priority)
                    color: row.done ? app.faint : app.accent
                    font.pixelSize: app.fsSubtitle
                    font.bold: true
                }
                Text {
                    visible: !row.editing
                    Layout.fillWidth: true
                    text: row.title
                    textFormat: Text.PlainText
                    // Said outright so right-to-left mirroring can turn it around.
                    horizontalAlignment: Text.AlignLeft
                    color: row.done ? app.faint : row.cursor ? app.cursorText : app.bright
                    font.pixelSize: app.fsSubtitle
                    font.strikeout: row.done
                    wrapMode: Text.Wrap
                    maximumLineCount: 4
                    elide: Text.ElideRight
                }
                TextInput {
                    id: editor
                    visible: row.editing
                    Layout.fillWidth: true
                    color: app.bright
                    selectionColor: app.ui.selectionFill
                    selectedTextColor: app.bright
                    font.pixelSize: app.fsSubtitle
                    wrapMode: TextInput.Wrap
                    horizontalAlignment: TextInput.AlignLeft
                    Keys.onReturnPressed: app.finishEdit(text, true)
                    Keys.onEnterPressed: app.finishEdit(text, true)
                    Keys.onTabPressed: backend.indent(row.rid)
                    Keys.onBacktabPressed: backend.outdent(row.rid)
                    // Clicking away keeps what was typed. Another window
                    // taking the keyboard for a moment (the pointer passing
                    // over it, with focus following the mouse) is not that:
                    // the typing goes on when the keyboard comes back.
                    onActiveFocusChanged: if (!activeFocus && row.editing && app.active) app.finishEdit(text, false)

                    Text {
                        visible: editor.text === "" && editor.preeditText === ""
                        text: qsTr("New Reminder")
                        color: app.faint
                        font: editor.font
                    }
                }
            }

            // What Nudge read out of the words so far.
            Text {
                id: preview
                readonly property string reading: row.editing && row.draft ? backend.quickPreview(editor.text) : ""
                visible: reading !== ""
                text: reading
                textFormat: Text.PlainText
                color: app.accent
                font.pixelSize: app.fsSmall
            }
            Text {
                visible: row.notes !== ""
                Layout.fillWidth: true
                text: row.notes
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignLeft
                color: row.done ? app.faint : app.dim
                font.pixelSize: app.fsSmall
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Flow {
                // Stands aside for the reading of a reminder being typed.
                visible: row.hasMeta && preview.reading === ""
                Layout.fillWidth: true
                spacing: 12

                Text {
                    visible: row.dueText !== ""
                    text: row.dueText
                    textFormat: Text.PlainText
                    color: row.overdue ? app.danger : app.dim
                    font.pixelSize: app.fsSmall
                }
                Row {
                    visible: row.repeatText !== ""
                    spacing: 4
                    Glyph { name: "repeat"; size: 10; color: app.faint; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: row.repeatText; textFormat: Text.PlainText; color: app.dim; font.pixelSize: app.fsSmall }
                }
                Text {
                    visible: row.tags !== ""
                    text: row.tags
                    textFormat: Text.PlainText
                    color: row.done ? app.faint : app.dim
                    font.pixelSize: app.fsSmall
                }
                Text {
                    visible: row.link !== ""
                    text: row.link
                    textFormat: Text.PlainText
                    color: app.dim
                    font.pixelSize: app.fsSmall
                    font.underline: true
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: backend.openLink(row.rid) }
                }
                Text {
                    visible: row.parentTitle !== ""
                    text: "↳ " + row.parentTitle
                    textFormat: Text.PlainText
                    color: app.faint
                    font.pixelSize: app.fsSmall
                }
                // Where it lives, with its list's own color for a mark.
                Row {
                    visible: row.listName !== ""
                    spacing: 5
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 6
                        height: 6
                        radius: Math.min(app.corner, 3)
                        color: app.swatch(row.tint)
                    }
                    Text {
                        text: row.listName
                        textFormat: Text.PlainText
                        color: app.faint
                        font.pixelSize: app.fsSmall
                    }
                }
            }
        }

        Glyph {
            visible: row.flagged
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 1
            name: "flag"
            size: 13
            filled: true
            color: app.swatch("orange")
        }
    }
}
