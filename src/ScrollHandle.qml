import QtQuick
import QtQuick.Controls.Basic

// A thin scroll bar that shows only while there is scrolling going on.
ScrollBar {
    id: bar
    padding: 2
    minimumSize: 0.08
    contentItem: Rectangle {
        implicitWidth: 4
        implicitHeight: 4
        radius: theme.radius > 0 ? 2 : 0
        color: Qt.alpha(look.colors.foreground, bar.pressed ? 0.5 : 0.28)
        opacity: bar.active && bar.size < 1 ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: look.reduceMotion ? 0 : 200 } }
    }
    background: null
}
