import QtQuick
import QtQuick.Controls.Basic

// A menu drawn like the Omarchy menu: the theme's background, a hairline
// border, and rows that take the accent when they are under the cursor.
Menu {
    id: menu
    readonly property var ui: look.colors

    padding: 1
    topPadding: 4
    bottomPadding: 4
    font.family: look.fontFamily
    font.pixelSize: look.fontSize
    delegate: AppMenuItem {}

    background: Rectangle {
        implicitWidth: 220
        color: menu.ui.menuBackground
        radius: theme.radius
        border.width: 1
        border.color: menu.ui.menuBorder
    }
}
