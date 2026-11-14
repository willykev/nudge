import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Everything the selected reminder holds, to read and to change, laid out
// like a panel of the Omarchy shell: a caption over each control.
Rectangle {
    id: pane
    property var app

    // The reminder the fields were filled from, and its id. Edits are saved
    // to it by id, and only load() moves it on, once what was typed is
    // saved, so text typed for one reminder can never land on another.
    property var item: ({})
    readonly property bool has: item.id !== undefined
    property var loadedId: 0

    readonly property var repeatUnits: ["never", "daily", "weekdays", "weekly", "monthly", "yearly"]
    readonly property var earlyMinutes: [0, 5, 15, 30, 60, 120, 1440, 2880, 10080]

    color: app.panel

    function focusDate() { dateField.forceActiveFocus(); dateField.selectAll() }

    // Saves whatever is typed but not yet saved. The selection is about to
    // change, or the window to close.
    function flush() {
        for (const field of [titleField, linkField, dateField, timeField, endField, tagsField])
            field.flush()
        notesArea.flush()
    }
    // Shows the selected reminder. Another one gets the fields only after
    // what was typed for the last is saved; saving can land back here, so
    // the selection is read again after. Then every field starts afresh,
    // even one that reads the same for both.
    function load() {
        if ((backend.selected.id || 0) !== loadedId)
            flush()
        const selected = backend.selected
        const another = (selected.id || 0) !== loadedId
        loadedId = selected.id || 0
        item = selected
        if (another) {
            for (const field of [titleField, linkField, dateField, timeField, endField, tagsField])
                field.showValue()
            notesArea.text = notesArea.value
        }
    }
    Connections {
        target: backend
        function onSelectedChanged() { pane.load() }
    }
    Component.onCompleted: load()

    // Small and bold, as the shell heads the sections of its panels.
    component Caption: Text {
        color: app.faint
        font.pixelSize: app.fsCaption
        font.bold: true
        horizontalAlignment: Text.AlignLeft
        Layout.fillWidth: true
        Layout.topMargin: 8
    }

    Text {
        visible: !pane.has
        anchors.centerIn: parent
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("Select a reminder to see its details")
        color: app.faint
        font.pixelSize: app.fsBody
    }

    Flickable {
        visible: pane.has
        anchors.fill: parent
        contentHeight: form.implicitHeight + 28
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollHandle {}

        ColumnLayout {
            id: form
            x: 16
            y: 8
            width: parent.width - 32
            spacing: 5

            Caption { text: qsTr("Title") }
            InputField {
                id: titleField
                Layout.fillWidth: true
                value: pane.item.title || ""
                save: text => backend.setTitle(pane.loadedId, text)
            }

            Caption { text: qsTr("Notes") }
            TextArea {
                id: notesArea
                property string value: pane.item.notes || ""
                Layout.fillWidth: true
                Layout.minimumHeight: 72
                font.family: look.fontFamily
                font.pixelSize: app.fsBody
                color: app.ink
                selectionColor: app.ui.selectionFill
                selectedTextColor: app.ink
                leftPadding: 10
                rightPadding: 10
                topPadding: 7
                bottomPadding: 7
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                background: Rectangle {
                    radius: app.corner
                    color: notesArea.activeFocus ? app.ui.focusFill : notesArea.hovered ? app.ui.hoverFill : app.ui.controlFill
                    border.width: look.controlBorderWidth
                    border.color: notesArea.activeFocus ? app.ui.focusBorder : notesArea.hovered ? app.ui.hoverBorder : app.ui.controlBorder
                }
                onValueChanged: text = value
                Component.onCompleted: text = value
                function flush() {
                    if (text !== value && pane.loadedId)
                        backend.setNotes(pane.loadedId, text)
                }
                onActiveFocusChanged: if (!activeFocus) flush()
            }

            Caption { text: qsTr("Link") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                InputField {
                    id: linkField
                    Layout.fillWidth: true
                    value: pane.item.url || ""
                    save: text => backend.setUrl(pane.loadedId, text)
                }
                PushButton {
                    visible: pane.item.hasLink === true
                    activeFocusOnTab: false
                    implicitWidth: 64
                    text: qsTr("Open")
                    onClicked: backend.openLink(pane.loadedId)
                }
            }

            Caption { text: qsTr("When") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                InputField {
                    id: dateField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Date")
                    value: pane.item.date || ""
                    save: text => backend.setDate(pane.loadedId, text)
                }
                Rectangle {
                    implicitWidth: 30
                    implicitHeight: 30
                    radius: app.corner
                    color: pickHover.hovered ? app.ui.hoverFill : app.ui.controlFill
                    border.width: look.controlBorderWidth
                    border.color: pickHover.hovered ? app.ui.hoverBorder : app.ui.controlBorder
                    Behavior on color { ColorAnimation { duration: app.ms(80) } }
                    Glyph { anchors.centerIn: parent; name: "calendar"; size: 13; color: pickHover.hovered ? app.ink : app.dim }
                    HoverHandler { id: pickHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: app.chooseDate(pane.loadedId, pane.item.dateIso) }
                }
                InputField {
                    id: timeField
                    Layout.preferredWidth: 92
                    placeholderText: qsTr("Time")
                    value: pane.item.time || ""
                    save: text => backend.setTime(pane.loadedId, text)
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 3
                spacing: 6
                PushButton { compact: true; activeFocusOnTab: false; text: qsTr("Today"); onClicked: backend.setDueIn(pane.loadedId, 0) }
                PushButton { compact: true; activeFocusOnTab: false; text: qsTr("Tomorrow"); onClicked: backend.setDueIn(pane.loadedId, 1) }
                PushButton { compact: true; activeFocusOnTab: false; text: qsTr("In a Week"); onClicked: backend.setDueIn(pane.loadedId, 7) }
                PushButton {
                    visible: pane.item.dateIso !== ""
                    compact: true
                    activeFocusOnTab: false
                    text: qsTr("No Date")
                    onClicked: backend.clearDue(pane.loadedId)
                }
            }

            Caption { text: qsTr("Repeat") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Dropdown {
                    id: repeatBox
                    Layout.fillWidth: true
                    model: [qsTr("Never"), qsTr("Daily"), qsTr("Weekdays"), qsTr("Weekly"), qsTr("Monthly"), qsTr("Yearly")]
                    currentIndex: Math.max(0, pane.repeatUnits.indexOf(pane.item.repeatUnit))
                    onActivated: index => backend.setRepeat(pane.loadedId, pane.repeatUnits[index], every.value)
                    onLetGo: app.focusList()
                }
                Stepper {
                    id: every
                    visible: repeatBox.currentIndex > 0 && repeatBox.currentIndex !== 2
                    from: 1
                    to: 999 // as far as a repeat goes, so no interval is cut down to fit
                    value: pane.item.repeatInterval || 1
                    onValueModified: backend.setRepeat(pane.loadedId, pane.item.repeatUnit, value)
                }
            }
            Text {
                visible: pane.item.repeatInterval > 1
                text: pane.item.repeatText || ""
                textFormat: Text.PlainText
                color: app.dim
                font.pixelSize: app.fsSmall
            }
            Caption { visible: endField.visible; text: qsTr("Ends") }
            InputField {
                id: endField
                visible: repeatBox.currentIndex > 0
                Layout.fillWidth: true
                value: pane.item.repeatEnd || ""
                save: text => backend.setRepeatEnd(pane.loadedId, text)
            }

            Caption { visible: pane.item.dateIso !== ""; text: qsTr("Early Reminder") }
            Dropdown {
                visible: pane.item.dateIso !== ""
                Layout.fillWidth: true
                model: [qsTr("None"), qsTr("5 minutes before"), qsTr("15 minutes before"), qsTr("30 minutes before"),
                        qsTr("1 hour before"), qsTr("2 hours before"), qsTr("1 day before"), qsTr("2 days before"),
                        qsTr("1 week before")]
                currentIndex: Math.max(0, pane.earlyMinutes.indexOf(pane.item.early))
                onActivated: index => backend.setEarly(pane.loadedId, pane.earlyMinutes[index])
                onLetGo: app.focusList()
            }

            Caption { text: qsTr("List") }
            Dropdown {
                Layout.fillWidth: true
                model: backend.lists
                textRole: "name"
                currentIndex: backend.lists.findIndex(list => list.id === pane.item.listId)
                onActivated: index => backend.moveToList(pane.loadedId, backend.lists[index].id)
                onLetGo: app.focusList()
            }

            Caption { text: qsTr("Priority") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Dropdown {
                    Layout.fillWidth: true
                    model: [qsTr("None"), qsTr("Low"), qsTr("Medium"), qsTr("High")]
                    currentIndex: pane.item.priority || 0
                    onActivated: index => backend.setPriority(pane.loadedId, index)
                    onLetGo: app.focusList()
                }
                Toggle {
                    text: qsTr("Flag")
                    checked: pane.item.flagged === true
                    onToggled: checked => backend.setFlagged(pane.loadedId, checked)
                }
            }

            Caption { text: qsTr("Tags") }
            InputField {
                id: tagsField
                Layout.fillWidth: true
                value: pane.item.tags || ""
                save: text => backend.setTags(pane.loadedId, text)
            }
        }
    }
}
