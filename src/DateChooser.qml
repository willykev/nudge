import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A month to click a day in. Typing a date in the field is the keyboard's way.
Popup {
    id: chooser
    property var app
    property var reminderId: 0
    property string selectedIso: ""
    // Reads the language, so the month and day names follow a switch.
    readonly property var language: { translator.language; return Qt.locale() }

    function iso(year, month, day) {
        return year + "-" + String(month + 1).padStart(2, "0") + "-" + String(day).padStart(2, "0")
    }
    function openFor(id, current) {
        reminderId = id
        selectedIso = current || ""
        const shown = current ? new Date(current + "T12:00:00") : new Date()
        grid.year = shown.getFullYear()
        grid.month = shown.getMonth()
        open()
    }
    function turn(delta) {
        const next = new Date(grid.year, grid.month + delta, 1)
        grid.year = next.getFullYear()
        grid.month = next.getMonth()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 300
    modal: true
    focus: true
    padding: 14
    Overlay.modal: Rectangle { color: Qt.alpha(chooser.app.paper, 0.7) }

    background: Rectangle {
        color: chooser.app.ui.cardBackground
        radius: chooser.app.corner
        border.width: look.controlBorderWidth
        border.color: chooser.app.ui.cardBorder
    }

    component Turn: Rectangle {
        property string icon
        signal triggered()
        implicitWidth: 26
        implicitHeight: 26
        radius: chooser.app.corner
        color: turnHover.hovered ? chooser.app.cursorFill : "transparent"
        Glyph { anchors.centerIn: parent; name: parent.icon; size: 13; color: chooser.app.dim }
        HoverHandler { id: turnHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: parent.triggered() }
    }

    contentItem: ColumnLayout {
        spacing: 8
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            Layout.fillWidth: true
            Turn { icon: "chevron-left"; onTriggered: chooser.turn(-1) }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: chooser.language.standaloneMonthName(grid.month) + " " + grid.year
                color: chooser.app.bright
                font.pixelSize: chooser.app.fsTitle
                font.bold: true
            }
            Turn { icon: "chevron-right"; onTriggered: chooser.turn(1) }
        }
        DayOfWeekRow {
            Layout.fillWidth: true
            locale: chooser.language
            delegate: Text {
                required property string shortName
                text: shortName
                color: chooser.app.faint
                font.pixelSize: chooser.app.fsSmall
                horizontalAlignment: Text.AlignHCenter
            }
        }
        MonthGrid {
            id: grid
            Layout.fillWidth: true
            locale: chooser.language
            delegate: Item {
                id: cell
                required property var model
                readonly property string iso: chooser.iso(model.year, model.month, model.day)
                readonly property bool picked: iso === chooser.selectedIso
                implicitWidth: 36
                implicitHeight: 30

                Rectangle {
                    anchors.centerIn: parent
                    width: 28
                    height: 26
                    radius: chooser.app.corner
                    // The day chosen is drawn as the cursor is everywhere
                    // else; today wears a hairline.
                    color: cell.picked ? chooser.app.cursorFill : cellHover.hovered ? chooser.app.ui.controlFill : "transparent"
                    border.width: cell.picked || cell.model.today ? 1 : 0
                    border.color: cell.picked ? chooser.app.accent : chooser.app.cursorBorder
                }
                Text {
                    anchors.centerIn: parent
                    text: cell.model.day
                    color: cell.picked ? chooser.app.cursorText : chooser.app.ink
                    font.bold: cell.model.today
                    opacity: cell.model.month === grid.month ? 1 : 0.35
                    font.pixelSize: chooser.app.fsBody
                }
                HoverHandler { id: cellHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        backend.setDateIso(chooser.reminderId, cell.iso)
                        chooser.close()
                    }
                }
            }
        }
    }
}
