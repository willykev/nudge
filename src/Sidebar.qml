import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Search, the built-in views, the user's lists, and the tags in use.
Rectangle {
    id: sidebar
    property var app
    property alias searchField: search

    color: app.panel

    Connections {
        target: backend
        function onSearchChanged() {
            if (search.text !== backend.search)
                search.text = backend.search
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Drawn like a field in the shell's own panels.
        Rectangle {
            Layout.fillWidth: true
            Layout.margins: 10
            Layout.topMargin: 12
            Layout.bottomMargin: 10
            implicitHeight: 30
            radius: app.corner
            color: search.activeFocus ? app.ui.focusFill : searchHover.hovered ? app.ui.hoverFill : app.ui.controlFill
            border.width: look.controlBorderWidth
            border.color: search.activeFocus ? app.ui.focusBorder : searchHover.hovered ? app.ui.hoverBorder : app.ui.controlBorder
            Behavior on color { ColorAnimation { duration: app.ms(80) } }

            HoverHandler { id: searchHover; cursorShape: Qt.IBeamCursor }
            Glyph {
                anchors.left: parent.left
                anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                name: "search"
                size: 12
                color: search.activeFocus ? app.ink : app.faint
            }
            TextInput {
                id: search
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                anchors.leftMargin: 29
                anchors.rightMargin: 28
                clip: true
                color: app.ink
                selectionColor: app.ui.selectionFill
                selectedTextColor: app.ink
                font.pixelSize: app.fsBody
                horizontalAlignment: TextInput.AlignLeft
                onTextEdited: backend.search = text
                // Down or Return carries on into what was found.
                Keys.onDownPressed: app.focusList()
                Keys.onReturnPressed: app.focusList()
                Keys.onEnterPressed: app.focusList()

                Text {
                    visible: search.text === "" && search.preeditText === ""
                    text: qsTr("Search")
                    color: app.faint
                    font: search.font
                }
            }
            // The key that gets here, until something is typed.
            Text {
                visible: search.text === "" && !search.activeFocus
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                anchors.rightMargin: 10
                text: "/"
                color: app.faint
                font.pixelSize: app.fsSmall
            }
            Item {
                visible: search.text !== ""
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                anchors.rightMargin: 5
                width: 20
                height: 20
                Glyph { anchors.centerIn: parent; name: "close"; size: 10; color: app.dim }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { backend.search = ""; app.focusList() } }
            }
        }

        ListView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: backend.sidebar
            currentIndex: -1
            ScrollBar.vertical: ScrollHandle {}

            // Small and bold, as the shell heads the sections of its panels.
            section.property: "group"
            section.delegate: Item {
                required property string section
                width: view.width
                height: section === "" ? 0 : 36
                Text {
                    visible: parent.section !== ""
                    anchors { left: parent.left; bottom: parent.bottom }
                    anchors.leftMargin: 20
                    anchors.bottomMargin: 7
                    text: parent.section === "lists" ? qsTr("My Lists") : qsTr("Tags")
                    color: app.faint
                    font.pixelSize: app.fsCaption
                    font.bold: true
                }
            }

            delegate: Item {
                id: entry
                required property string key
                required property string group
                required property string name
                required property string tint
                required property string icon
                required property int count

                // The view on screen, and whether the keys are on it.
                readonly property bool current: backend.viewKey === key
                readonly property bool cursor: current && app.pane === "sidebar"

                width: view.width
                height: 28

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    radius: app.corner
                    color: entry.current ? app.cursorFill : entryHover.hovered ? app.ui.controlFill : "transparent"
                    border.width: entry.cursor ? look.controlBorderWidth : 0
                    border.color: app.cursorBorder
                    Behavior on color { ColorAnimation { duration: app.ms(60) } }
                }
                HoverHandler { id: entryHover }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: app.showView(entry.key)
                    onDoubleTapped: if (entry.group === "lists") app.editList(entry.key)
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: if (entry.group === "lists") { app.showView(entry.key); app.openListMenu() }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    spacing: 9

                    Item {
                        implicitWidth: 14
                        implicitHeight: 14
                        // A list is known by its color, the one mark of its
                        // own; everything else is the color of the text.
                        Rectangle {
                            visible: entry.group === "lists"
                            anchors.centerIn: parent
                            width: 7
                            height: 7
                            radius: Math.min(app.corner, 3.5)
                            color: app.swatch(entry.tint)
                        }
                        Glyph {
                            visible: entry.group !== "lists"
                            anchors.centerIn: parent
                            name: entry.icon
                            size: 13
                            color: entry.current ? app.cursorText : app.dim
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: entry.name
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        // The view on screen takes the accent, as the selected
                        // row of the Omarchy menu does.
                        color: entry.current ? app.cursorText : app.ink
                        font.pixelSize: app.fsBody
                    }
                    Text {
                        visible: entry.count > 0
                        // Reads the language so the digits follow a switch.
                        text: { translator.language; return entry.count.toLocaleString(Qt.locale(), "f", 0) }
                        color: entry.current ? app.dim : app.faint
                        font.pixelSize: app.fsSmall
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: app.rule
        }
        Item {
            Layout.fillWidth: true
            implicitHeight: 34

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                radius: app.corner
                color: addHover.hovered ? app.cursorFill : "transparent"
                Behavior on color { ColorAnimation { duration: app.ms(60) } }
            }
            Row {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                anchors.leftMargin: 20
                spacing: 9
                Glyph { name: "plus"; size: 12; color: app.dim; anchors.verticalCenter: parent.verticalCenter }
                Text { text: qsTr("New List"); color: app.dim; font.pixelSize: app.fsBody; anchors.verticalCenter: parent.verticalCenter }
            }
            HoverHandler { id: addHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: app.editList("") }
        }
    }
}
