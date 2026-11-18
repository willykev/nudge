import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQml.Models
import Qt.labs.qmlmodels

ApplicationWindow {
    id: win
    width: 1120
    height: 720
    minimumWidth: 760
    minimumHeight: 480
    visible: true
    title: "Nudge"
    color: paper

    // Everything the window is drawn with comes from the Omarchy theme, by
    // role, and follows it as it changes: `look` holds the palette and the
    // shell's control states, `theme` the rounding of the desktop's windows.
    readonly property var ui: look.colors
    readonly property color paper: ui.background  // where the reminders are
    readonly property color panel: ui.panel       // the sidebar and the details, a shade apart
    readonly property color ink: ui.foreground
    readonly property color bright: ui.bright
    readonly property color dim: ui.dim
    readonly property color faint: ui.faint
    readonly property color hairline: ui.hairline
    readonly property color rule: Qt.alpha(ui.foreground, 0.12) // the shell's separator
    readonly property color hover: ui.hoverFill
    readonly property color chosen: ui.selectedFill
    readonly property color accent: ui.accent
    readonly property color danger: ui.danger
    // The row the keys are on, as the Omarchy menu draws its own.
    readonly property color cursorFill: ui.menuSelectedFill
    readonly property color cursorBorder: ui.hoverBorder
    readonly property color cursorText: ui.menuSelectedText
    readonly property int corner: theme.radius
    readonly property int gutter: 28 // where the list's content starts, headings and boxes alike

    // The shell's type scale, from the theme's base size.
    readonly property int fsCaption: Math.round(look.fontSize * 0.833)
    readonly property int fsSmall: Math.round(look.fontSize * 0.917)
    readonly property int fsBody: look.fontSize
    readonly property int fsSubtitle: Math.round(look.fontSize * 1.083)
    readonly property int fsTitle: Math.round(look.fontSize * 1.167)
    readonly property int fsHeading: Math.round(look.fontSize * 1.333)
    readonly property int fsDisplay: Math.round(look.fontSize * 1.667)

    font.family: look.fontFamily
    font.pixelSize: fsBody
    // What stock controls (scroll bars, the calendar) fall back on.
    palette.window: paper
    palette.base: paper
    palette.text: ink
    palette.windowText: ink
    palette.button: ui.raised
    palette.buttonText: ink
    palette.highlight: accent
    palette.highlightedText: ui.accentInk
    palette.placeholderText: faint
    palette.mid: faint
    palette.dark: dim
    // Controls, menus, and dialogs take their direction and digits from this,
    // so it follows the language as it is switched.
    locale: { translator.language; return Qt.locale() }

    property bool helpVisible: false
    property bool detailsVisible: false
    property string pane: "list" // which side the arrow keys drive: "list" or "sidebar"
    property var editingId: 0    // the reminder whose title is being typed in the list
    property var activeEditor: null // and the field it is being typed in

    // Dialogs and menus live in the overlay while they are open, and have
    // the keyboard to themselves.
    readonly property bool popupOpen: Overlay.overlay.children.length > 0
    // Single keys act on the reminders only while nothing else wants them:
    // not while typing in a field or working a control in the details pane.
    readonly property bool keys: !helpVisible && !popupOpen && !editingId
                                 && (listView.activeFocus || activeFocusItem === contentItem || !activeFocusItem)
    readonly property bool onReminder: keys && pane === "list" && backend.selectedId > 0

    // A color by the name a list or view carries: one of the theme's own
    // (red, blue, ...), a role (dim), or a plain "#rrggbb" from before.
    function swatch(name) {
        return name === "" ? ui.accent : (look.swatches[name] || ui[name] || name)
    }
    // Nothing moves when Hyprland's own animations are off.
    function ms(duration) { return look.reduceMotion ? 0 : duration }

    // A passing word for the status line at the foot of the window.
    property string status: ""
    function say(message) {
        status = message
        statusTimer.restart()
    }
    Timer { id: statusTimer; interval: 4200; onTriggered: win.status = "" }

    // The list is a focus scope: handing it the keyboard hands it on to
    // whatever inside it had it last, so that has to let go first.
    function focusList() {
        pane = "list"
        if (editingId)
            finishEdit(activeEditor ? activeEditor.text : "", false)
        listView.forceActiveFocus()
    }
    // A title being typed is finished before the selection or the view moves
    // on, so a new reminder's title is part of the step that added it, and
    // one left blank is gone without a trace. Clicking its own row just
    // finishes it.
    function select(id) {
        const editing = editingId
        focusList()
        if (id !== editing)
            backend.selectedId = id
    }
    function showView(key) {
        focusList()
        backend.viewKey = key
    }
    function step(delta) {
        if (pane === "sidebar")
            backend.stepView(delta)
        else
            backend.selectStep(delta)
    }
    function newReminder() {
        focusList()
        const id = backend.addReminder()
        if (id)
            editingId = id
    }
    // Starts a new reminder with these words typed in it, for pictures of
    // one being written (nudge open --type). They go in once the row has
    // its field, which it sets up as it starts.
    property string typed: ""
    function type(words) {
        typed = words
        newReminder()
    }
    onActiveEditorChanged: if (activeEditor && typed !== "") Qt.callLater(() => {
        if (activeEditor)
            activeEditor.text = typed
        typed = ""
    })
    function editTitle() {
        const row = backend.selectedRow
        if (row < 0)
            return
        // A row scrolled out of sight has no field to type in until it is
        // back in view.
        listView.positionViewAtIndex(row, ListView.Contain)
        editingId = backend.selectedId
    }
    // Return while typing a new reminder saves it and starts the next, so a
    // list can be written out one line after another. Nothing here takes the
    // keyboard: the field letting go of it leaves it with the list, and a
    // field clicked into keeps it.
    function finishEdit(text, another) {
        const id = editingId
        if (!id)
            return
        const draft = backend.draftId === id
        editingId = 0
        activeEditor = null // so a later edit never saves this one's text
        // A new reminder left blank for a field of the details showing it is
        // still being added there: it stays selected for the details to fill
        // in, until the selection or the view moves on.
        if (draft && !another && text.trim() === "" && backend.selectedId === id && within(activeFocusItem, details)) {
            backend.holdDraft(id)
            return
        }
        backend.commitTitle(id, text)
        if (another && draft && text.trim() !== "")
            newReminder()
    }
    function within(item, ancestor) {
        for (; item; item = item.parent)
            if (item === ancestor)
                return true
        return false
    }
    function editList(key) {
        if (key === "")
            listEditor.openFor(0)
        else if (key.startsWith("list:"))
            listEditor.openFor(Number(key.slice(5)))
    }
    function deleteList() {
        const id = backend.viewListId
        if (!id)
            return
        const info = backend.listInfo(id)
        if (info.total === 0) {
            backend.removeList(id)
            return
        }
        ask.confirm(qsTr("Delete “%1”?").arg(info.name),
                    qsTr("Its %n reminders are deleted with it. Undo brings them back.", "", info.total),
                    qsTr("Delete"), () => backend.removeList(id))
    }
    // What is asked about is what is deleted: the reminders counted as the
    // question goes up, not whatever has been completed by the answer.
    function clearCompleted() {
        const ids = backend.completedIds()
        if (ids.length === 0) {
            say(qsTr("There are no completed reminders to delete"))
            return
        }
        ask.confirm(qsTr("Delete %n completed reminders?", "", ids.length), qsTr("Undo brings them back."),
                    qsTr("Delete"), () => backend.clearCompleted(ids))
    }
    function newSection() {
        if (backend.viewKind !== "list") {
            say(qsTr("Sections belong to a list. Open one first."))
            return
        }
        ask.askText(qsTr("New Section"), "", qsTr("Add"), text => backend.addSection(text))
    }
    function renameSection(id) {
        ask.askText(qsTr("Rename Section"), backend.sectionName(id), qsTr("Save"),
                    text => backend.renameSection(id, text))
    }
    function chooseDate(id, iso) { dates.openFor(id, iso) }
    function openRowMenu() { rowMenu.popup() }
    function openListMenu() { listMenu.popup() }
    function typeDate() {
        detailsVisible = true
        details.focusDate()
    }
    // Opens the window on one of its dialogs, for pictures of them.
    function show(what) {
        if (what === "list")
            listEditor.openFor(0)
        else if (what === "date")
            dates.openFor(backend.selectedId, backend.selected.dateIso)
        else if (what === "move")
            movePicker.openFor(backend.selectedId)
        else if (what === "menu")
            rowMenu.popup(listView, 300, 120)
        else if (what === "delete")
            deleteList()
    }
    function goBack() {
        if (helpVisible)
            helpVisible = false
        else if (backend.search !== "" || sidebar.searchField.activeFocus) {
            backend.search = ""
            focusList()
        } else if (editingId || !listView.activeFocus || pane !== "list")
            focusList() // a field being typed in saves as it loses the keyboard
        else if (detailsVisible)
            detailsVisible = false
    }

    Component.onCompleted: listView.forceActiveFocus()
    // Closing the window mid-word keeps the word, and settles a new reminder
    // the details were filling in.
    onClosing: {
        focusList()
        details.flush()
        backend.settleDraft()
    }

    Connections {
        target: backend
        function onNotice(text) { win.say(text) }
        function onRaise() {
            win.raise()
            win.requestActivate()
        }
        function onSelectedRowChanged() {
            if (backend.selectedRow >= 0)
                listView.positionViewAtIndex(backend.selectedRow, ListView.Contain)
        }
    }

    // ---- Keys ----------------------------------------------------------------

    Shortcut { sequences: ["?", "Shift+?", "F1"]; context: Qt.ApplicationShortcut; enabled: win.keys || win.helpVisible; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.close() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: !win.popupOpen; onActivated: win.goBack() }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: !win.popupOpen; onActivated: backend.undo() }
    // Named outright: the platform's Redo keys vary, and a key bound twice
    // answers to neither.
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: !win.popupOpen; onActivated: backend.redo() }

    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.toggleCompleted(backend.selectedId) }
    Shortcut { sequences: ["N", "Ctrl+N"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.newReminder() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane === "sidebar" ? win.editList(backend.viewKey) : win.editTitle() }
    Shortcut { sequences: ["Delete", "Backspace"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane === "sidebar" ? win.deleteList() : backend.remove(backend.selectedId) }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.detailsVisible = !win.detailsVisible }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.toggleFlag(backend.selectedId) }
    Shortcut { sequence: "0"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setPriority(backend.selectedId, 0) }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setPriority(backend.selectedId, 1) }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setPriority(backend.selectedId, 2) }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setPriority(backend.selectedId, 3) }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.indent(backend.selectedId) }
    Shortcut { sequence: "Shift+Tab"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.outdent(backend.selectedId) }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.toggleCollapsed(backend.selectedId) }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: movePicker.openFor(backend.selectedId) }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.openLink(backend.selectedId) }
    Shortcut { sequences: ["Alt+Up", "Shift+K"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane === "sidebar" ? backend.moveList(backend.viewListId, -1) : backend.moveReminder(backend.selectedId, -1) }
    Shortcut { sequences: ["Alt+Down", "Shift+J"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane === "sidebar" ? backend.moveList(backend.viewListId, 1) : backend.moveReminder(backend.selectedId, 1) }

    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setDueIn(backend.selectedId, 0) }
    Shortcut { sequence: "Shift+T"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setDueIn(backend.selectedId, 1) }
    Shortcut { sequence: "W"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.setDueIn(backend.selectedId, 7) }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: win.typeDate() }
    Shortcut { sequence: "Shift+D"; context: Qt.ApplicationShortcut; enabled: win.onReminder; onActivated: backend.clearDue(backend.selectedId) }

    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.step(-1) }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.step(1) }
    Shortcut { sequences: ["Left", "H"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane = "sidebar" }
    Shortcut { sequences: ["Right", "L"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane = "list" }
    Shortcut { sequence: "PgUp"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.step(-10) }
    Shortcut { sequence: "PgDown"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.step(10) }
    Shortcut { sequences: ["Home", "G"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane === "sidebar" ? backend.stepView(-1000) : backend.selectEdge(false) }
    Shortcut { sequences: ["End", "Shift+G"]; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.pane === "sidebar" ? backend.stepView(1000) : backend.selectEdge(true) }
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: { sidebar.searchField.forceActiveFocus(); sidebar.searchField.selectAll() } }
    Shortcut { sequences: [StandardKey.Find]; context: Qt.ApplicationShortcut; enabled: !win.popupOpen && !win.helpVisible; onActivated: { sidebar.searchField.forceActiveFocus(); sidebar.searchField.selectAll() } }

    Shortcut { sequence: "Shift+N"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.editList("") }
    Shortcut { sequence: "Shift+S"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.newSection() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: backend.cycleSort() }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: backend.showCompleted = !backend.showCompleted }
    Shortcut { sequence: "Shift+C"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: win.clearCompleted() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: win.keys; onActivated: backend.setBackground(!backend.backgroundOn) }

    // ---- Pieces ----------------------------------------------------------------

    component ToolIcon: Rectangle {
        id: tool
        property string icon
        property bool active: false
        signal triggered()

        implicitWidth: 28
        implicitHeight: 28
        radius: win.corner
        color: toolHover.hovered ? win.cursorFill : "transparent"
        border.width: toolHover.hovered ? look.controlBorderWidth : 0
        border.color: win.cursorBorder
        Behavior on color { ColorAnimation { duration: win.ms(60) } }

        Glyph { anchors.centerIn: parent; name: tool.icon; size: 15; color: tool.active ? win.accent : toolHover.hovered ? win.ink : win.dim }
        HoverHandler { id: toolHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: tool.triggered() }
    }

    // A strip across the top of the reminders for something that needs saying.
    component Banner: Rectangle {
        id: banner
        property string text
        property string actionText: ""
        property string dismissText: ""
        property bool urgent: false
        signal action()
        signal dismiss()

        Layout.fillWidth: true
        implicitHeight: Math.max(bannerText.implicitHeight, 30) + 14
        color: banner.urgent ? Qt.alpha(win.danger, 0.1) : win.ui.controlFill

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: banner.urgent ? Qt.alpha(win.danger, 0.5) : win.rule
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: win.gutter
            anchors.rightMargin: 14
            spacing: 8

            Text {
                id: bannerText
                Layout.fillWidth: true
                text: banner.text
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignLeft
                color: banner.urgent ? win.danger : win.ink
                font.pixelSize: win.fsBody
                wrapMode: Text.Wrap
            }
            PushButton {
                visible: banner.dismissText !== ""
                activeFocusOnTab: false
                text: banner.dismissText
                onClicked: banner.dismiss()
            }
            PushButton {
                visible: banner.actionText !== ""
                activeFocusOnTab: false
                primary: true
                text: banner.actionText
                onClicked: banner.action()
            }
        }
    }

    // A key and what it does, for the line at the foot of the window.
    component Hint: Row {
        property string key
        property string does
        spacing: 6
        Text { text: parent.key; color: win.ink; font.pixelSize: win.fsSmall }
        Text { text: parent.does; color: win.faint; font.pixelSize: win.fsSmall }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Sidebar {
                id: sidebar
                app: win
                Layout.preferredWidth: 236
                Layout.fillHeight: true
            }
            Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: win.rule }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Banner {
                    visible: backend.problem !== ""
                    urgent: true
                    text: backend.problem
                }
                Banner {
                    visible: backend.backgroundAsk && backend.problem === ""
                    text: qsTr("Reminders only arrive while Nudge is open. Let them arrive when it is closed too?")
                    actionText: qsTr("Turn On")
                    dismissText: qsTr("Not Now")
                    onAction: backend.setBackground(true)
                    onDismiss: backend.dismissBackgroundAsk()
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: win.gutter
                    Layout.rightMargin: 14
                    Layout.topMargin: 14
                    Layout.bottomMargin: 12
                    spacing: 2

                    Item {
                        Layout.fillWidth: true
                        implicitHeight: viewTitle.implicitHeight

                        // Headings are the theme's own text color, as in the
                        // shell's panels. Color stays with the small marks: a
                        // list's dot, a flag, an overdue date.
                        Text {
                            id: viewTitle
                            anchors.left: parent.left
                            width: Math.min(implicitWidth, parent.width - viewCount.implicitWidth - 12)
                            text: backend.viewTitle
                            textFormat: Text.PlainText
                            horizontalAlignment: Text.AlignLeft
                            elide: Text.ElideRight
                            color: win.bright
                            font.pixelSize: win.fsDisplay
                            font.bold: true
                        }
                        Text {
                            id: viewCount
                            visible: backend.viewKind !== "completed" && backend.openCount > 0
                            anchors.left: viewTitle.right
                            anchors.leftMargin: 12
                            anchors.baseline: viewTitle.baseline
                            // Reads the language so the digits follow a switch.
                            text: { translator.language; return backend.openCount.toLocaleString(Qt.locale(), "f", 0) }
                            color: win.faint
                            font.pixelSize: win.fsHeading
                        }
                    }
                    ToolIcon { icon: "plus"; onTriggered: win.newReminder() }
                    ToolIcon {
                        visible: backend.viewKind === "list"
                        icon: "sort"
                        active: backend.sortMode !== "manual"
                        onTriggered: sortMenu.popup()
                    }
                    ToolIcon { icon: "eye"; active: backend.showCompleted; onTriggered: backend.showCompleted = !backend.showCompleted }
                    ToolIcon { icon: "info"; active: win.detailsVisible; onTriggered: win.detailsVisible = !win.detailsVisible }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: win.rule }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        id: listView
                        objectName: "reminders"
                        anchors.fill: parent
                        model: backend.model
                        focus: true
                        // Selection is the backend's. A current item would
                        // take the keyboard for itself whenever rows change,
                        // and the arrows (Down while typing a title too)
                        // would make one.
                        currentIndex: -1
                        keyNavigationEnabled: false
                        clip: true
                        reuseItems: true
                        cacheBuffer: 480
                        boundsBehavior: Flickable.StopAtBounds
                        topMargin: 8
                        bottomMargin: 20
                        ScrollBar.vertical: ScrollHandle {}

                        delegate: DelegateChooser {
                            role: "kind"
                            DelegateChoice { roleValue: "heading"; GroupHeading { app: win } }
                            DelegateChoice { roleValue: "reminder"; ReminderRow { app: win } }
                        }

                        // Rows slide to make room and to close up, so the eye
                        // can follow a reminder that moved.
                        add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: win.ms(140) } }
                        displaced: Transition { NumberAnimation { properties: "y"; duration: win.ms(160); easing.type: Easing.OutCubic } }
                        move: Transition { NumberAnimation { properties: "y"; duration: win.ms(160); easing.type: Easing.OutCubic } }
                    }

                    Column {
                        visible: listView.count === 0
                        anchors.centerIn: parent
                        spacing: 6
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: backend.viewKind === "search" ? qsTr("Nothing found")
                                : backend.viewKind === "completed" ? qsTr("Nothing completed yet")
                                : qsTr("No reminders")
                            color: win.dim
                            font.pixelSize: win.fsHeading
                        }
                        Text {
                            visible: backend.viewKind !== "search" && backend.viewKind !== "completed"
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("Press N to add one, or ? for every shortcut")
                            color: win.faint
                            font.pixelSize: win.fsBody
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: win.rule }
                Item {
                    Layout.fillWidth: true
                    implicitHeight: 26
                    clip: true

                    Row {
                        visible: win.status === ""
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        anchors.leftMargin: win.gutter
                        spacing: 18
                        Hint { key: "N"; does: qsTr("New Reminder") }
                        Hint { key: "Space"; does: qsTr("Complete") }
                        Hint { key: "Return"; does: qsTr("Rename") }
                        Hint { key: "I"; does: qsTr("Details") }
                        Hint { key: "/"; does: qsTr("Search") }
                        Hint { key: "?"; does: qsTr("Shortcuts") }
                    }
                    Text {
                        visible: win.status !== ""
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        anchors.leftMargin: win.gutter
                        anchors.rightMargin: 14
                        text: win.status
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        color: win.ink
                        font.pixelSize: win.fsSmall
                    }
                }
            }

            Rectangle { visible: win.detailsVisible; Layout.preferredWidth: 1; Layout.fillHeight: true; color: win.rule }
            Details {
                id: details
                app: win
                visible: win.detailsVisible
                Layout.preferredWidth: 300
                Layout.fillHeight: true
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            app: win
        }
    }

    // ---- Dialogs and menus ---------------------------------------------------------

    ListEditor { id: listEditor; app: win }
    AskBox { id: ask; app: win }
    MovePicker { id: movePicker; app: win }
    DateChooser { id: dates; app: win }

    AppMenu {
        id: rowMenu
        readonly property var item: backend.selected
        // Runs once the menu has closed and handed the keyboard back, so a
        // field opened from it is not robbed of its focus.
        property var after: null
        onClosed: {
            const run = after
            after = null
            if (run)
                run()
        }

        AppMenuItem { text: rowMenu.item.done ? qsTr("Reopen") : qsTr("Complete"); onTriggered: backend.toggleCompleted(backend.selectedId) }
        AppMenuItem { text: qsTr("Rename"); onTriggered: rowMenu.after = () => win.editTitle() }
        AppMenuItem { text: qsTr("Details"); onTriggered: win.detailsVisible = true }
        AppMenuSeparator {}
        AppMenuItem { text: rowMenu.item.flagged ? qsTr("Remove Flag") : qsTr("Flag"); onTriggered: backend.toggleFlag(backend.selectedId) }
        AppMenu {
            title: qsTr("Priority")
            AppMenuItem { text: qsTr("None"); onTriggered: backend.setPriority(backend.selectedId, 0) }
            AppMenuItem { text: qsTr("Low"); onTriggered: backend.setPriority(backend.selectedId, 1) }
            AppMenuItem { text: qsTr("Medium"); onTriggered: backend.setPriority(backend.selectedId, 2) }
            AppMenuItem { text: qsTr("High"); onTriggered: backend.setPriority(backend.selectedId, 3) }
        }
        AppMenu {
            title: qsTr("Due")
            AppMenuItem { text: qsTr("Today"); onTriggered: backend.setDueIn(backend.selectedId, 0) }
            AppMenuItem { text: qsTr("Tomorrow"); onTriggered: backend.setDueIn(backend.selectedId, 1) }
            AppMenuItem { text: qsTr("In a Week"); onTriggered: backend.setDueIn(backend.selectedId, 7) }
            AppMenuItem { text: qsTr("Pick a date"); onTriggered: rowMenu.after = () => win.chooseDate(backend.selectedId, backend.selected.dateIso) }
            AppMenuItem { text: qsTr("No Date"); onTriggered: backend.clearDue(backend.selectedId) }
        }
        AppMenuItem { text: qsTr("Move to List"); onTriggered: rowMenu.after = () => movePicker.openFor(backend.selectedId) }
        AppMenu {
            id: sectionMenu
            title: qsTr("Move to Section")
            enabled: backend.sections.length > 0
            AppMenuItem { text: qsTr("No Section"); onTriggered: backend.moveToSection(backend.selectedId, 0) }
            Instantiator {
                model: backend.sections
                delegate: AppMenuItem {
                    required property var modelData
                    text: modelData.name
                    onTriggered: backend.moveToSection(backend.selectedId, modelData.id)
                }
                onObjectAdded: (index, object) => sectionMenu.insertItem(index + 1, object)
                onObjectRemoved: (index, object) => sectionMenu.removeItem(object)
            }
        }
        AppMenuSeparator {}
        AppMenuItem { text: qsTr("Delete"); onTriggered: backend.remove(backend.selectedId) }
    }

    AppMenu {
        id: listMenu
        property var after: null
        onClosed: {
            const run = after
            after = null
            if (run)
                run()
        }

        AppMenuItem { text: qsTr("Edit List"); onTriggered: listMenu.after = () => win.editList(backend.viewKey) }
        AppMenuItem { text: qsTr("New Section"); onTriggered: listMenu.after = () => win.newSection() }
        AppMenuItem { text: qsTr("Delete Completed"); onTriggered: listMenu.after = () => win.clearCompleted() }
        AppMenuSeparator {}
        AppMenuItem { text: qsTr("Delete List"); onTriggered: listMenu.after = () => win.deleteList() }
    }

    AppMenu {
        id: sortMenu
        Repeater {
            model: [
                { mode: "manual", name: qsTr("Manual Order") },
                { mode: "due", name: qsTr("Due Date") },
                { mode: "priority", name: qsTr("Priority") },
                { mode: "title", name: qsTr("Title") },
                { mode: "created", name: qsTr("Date Created") }
            ]
            AppMenuItem {
                required property var modelData
                text: modelData.name
                checkable: true
                checked: backend.sortMode === modelData.mode
                onTriggered: backend.sortMode = modelData.mode
            }
        }
    }
}
