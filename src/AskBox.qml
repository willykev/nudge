import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One small dialog for two questions: "what should it be called?" (with a
// text field) and "are you sure?" (without).
Popup {
    id: box
    property var app
    property string heading: ""
    property string message: ""
    property string actionText: ""
    property bool wantsText: false
    property bool destructive: false
    property var done: null

    function askText(title, text, action, callback) {
        heading = title
        message = ""
        actionText = action
        wantsText = true
        destructive = false
        done = callback
        input.text = text
        open()
        input.forceActiveFocus()
        input.selectAll()
    }
    function confirm(title, body, action, callback) {
        heading = title
        message = body
        actionText = action
        wantsText = false
        destructive = true
        done = callback
        open()
        accept.forceActiveFocus()
    }
    function finish() {
        const callback = done
        const text = input.text
        done = null
        close()
        if (callback)
            callback(text)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 380
    modal: true
    focus: true
    padding: 18
    Overlay.modal: Rectangle { color: Qt.alpha(box.app.paper, 0.7) }

    background: Rectangle {
        color: box.app.ui.cardBackground
        radius: box.app.corner
        border.width: look.controlBorderWidth
        border.color: box.destructive ? box.app.danger : box.app.ui.cardBorder
    }

    contentItem: ColumnLayout {
        spacing: 10
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Text {
            Layout.fillWidth: true
            text: box.heading
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
            color: box.app.bright
            font.pixelSize: box.app.fsTitle
            font.bold: true
        }
        Text {
            visible: box.message !== ""
            Layout.fillWidth: true
            text: box.message
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
            color: box.app.dim
            font.pixelSize: box.app.fsBody
        }
        InputField {
            id: input
            visible: box.wantsText
            Layout.fillWidth: true
            placeholderText: qsTr("Name")
            onAccepted: box.finish()
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 10
            Item { Layout.fillWidth: true }
            // Left and Right move between the two, as in the shell's own dialogs.
            PushButton {
                id: cancel
                text: qsTr("Cancel")
                onClicked: box.close()
                Keys.onRightPressed: accept.forceActiveFocus()
                Keys.onLeftPressed: accept.forceActiveFocus()
            }
            PushButton {
                id: accept
                primary: !box.destructive
                danger: box.destructive
                text: box.actionText
                onClicked: box.finish()
                Keys.onRightPressed: cancel.forceActiveFocus()
                Keys.onLeftPressed: cancel.forceActiveFocus()
            }
        }
    }
}
