import QtQuick
import QtQuick.Controls.Basic

MenuItem {
    id: item
    readonly property var ui: look.colors

    implicitHeight: 28
    leftPadding: 12
    rightPadding: 12
    font.family: look.fontFamily
    font.pixelSize: look.fontSize

    contentItem: Text {
        leftPadding: item.checkable ? 18 : 0
        rightPadding: item.subMenu ? 16 : 0
        text: item.text
        textFormat: Text.PlainText
        font: item.font
        color: !item.enabled ? item.ui.faint : item.highlighted ? item.ui.menuSelectedText : item.ui.menuText
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignLeft
        elide: Text.ElideRight
    }
    indicator: Glyph {
        visible: item.checkable && item.checked
        x: item.mirrored ? item.width - width - 10 : 10
        y: (item.height - height) / 2
        name: "check"
        size: 12
        color: item.ui.accent
    }
    arrow: Glyph {
        visible: item.subMenu !== null
        x: item.mirrored ? 8 : item.width - width - 8
        y: (item.height - height) / 2
        name: item.mirrored ? "chevron-left" : "chevron-right"
        size: 12
        color: item.ui.dim
    }
    background: Rectangle {
        implicitWidth: 220
        implicitHeight: 28
        color: item.highlighted ? item.ui.menuSelectedFill : "transparent"
    }
}
