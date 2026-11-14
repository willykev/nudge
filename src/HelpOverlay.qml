import QtQuick
import QtQuick.Layouts

// Every shortcut, on one sheet over the window.
Rectangle {
    id: sheet
    property var app

    readonly property var groups: [
        { title: qsTr("Reminders"), keys: [
            ["N", qsTr("New reminder")],
            ["Space", qsTr("Complete or reopen")],
            ["Return", qsTr("Rename")],
            ["I", qsTr("Show or hide details")],
            ["F", qsTr("Flag")],
            ["1  2  3  0", qsTr("Priority: low, medium, high, none")],
            ["Tab", qsTr("Make a subtask")],
            ["Shift+Tab", qsTr("Back to a reminder of its own")],
            ["X", qsTr("Fold or unfold subtasks")],
            ["M", qsTr("Move to another list")],
            ["Alt+↑  Alt+↓", qsTr("Move up or down")],
            ["O", qsTr("Open its link")],
            ["Delete", qsTr("Delete")]
        ] },
        { title: qsTr("Dates"), keys: [
            ["T", qsTr("Due today")],
            ["Shift+T", qsTr("Due tomorrow")],
            ["W", qsTr("Due in a week")],
            ["D", qsTr("Type a date")],
            ["Shift+D", qsTr("No date")]
        ] },
        { title: qsTr("Getting around"), keys: [
            ["↑  ↓", qsTr("Previous or next")],
            ["←  →", qsTr("Lists or reminders")],
            ["G  Shift+G", qsTr("First or last")],
            ["/", qsTr("Search")],
            ["Esc", qsTr("Back to the reminders")]
        ] },
        { title: qsTr("Lists"), keys: [
            ["Shift+N", qsTr("New list")],
            ["Shift+S", qsTr("New section")],
            ["S", qsTr("Change the sort order")],
            ["C", qsTr("Show or hide completed")],
            ["Shift+C", qsTr("Delete completed")]
        ] },
        { title: qsTr("Nudge"), keys: [
            ["Ctrl+Z", qsTr("Undo")],
            ["Ctrl+Shift+Z", qsTr("Redo")],
            ["B", qsTr("Reminders in the background")],
            ["?", qsTr("This sheet")],
            ["Q", qsTr("Quit")]
        ] }
    ]

    color: Qt.alpha(app.paper, 0.96)

    // Swallows clicks and the wheel so the window underneath stays put.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: sheet.app.helpVisible = false
        onWheel: wheel => wheel.accepted = true
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 32
        contentHeight: columns.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Flow {
            id: columns
            width: parent.width
            spacing: 36

            Repeater {
                model: sheet.groups
                Column {
                    id: block
                    required property var modelData
                    width: 300
                    spacing: 7

                    Text {
                        text: block.modelData.title
                        color: sheet.app.accent
                        font.pixelSize: sheet.app.fsSmall
                        font.bold: true
                        bottomPadding: 4
                    }
                    Repeater {
                        model: block.modelData.keys
                        RowLayout {
                            required property var modelData
                            width: block.width
                            spacing: 12
                            // Keys keep their names in every language.
                            Text {
                                Layout.preferredWidth: 112
                                text: modelData[0]
                                color: sheet.app.bright
                                font.pixelSize: sheet.app.fsBody
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData[1]
                                horizontalAlignment: Text.AlignLeft
                                color: sheet.app.dim
                                font.pixelSize: sheet.app.fsBody
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
}
