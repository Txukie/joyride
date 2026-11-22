// A button for pad, keys and mouse: a clear focus ring, Return and Enter
// press it as Space does.
import QtQuick
import QtQuick.Controls.Basic

Button {
    id: b
    property color accent: "#f0b429"
    property bool primary: false
    font.pixelSize: 22
    padding: 16
    leftPadding: 28
    rightPadding: 28
    focusPolicy: Qt.StrongFocus
    Keys.onReturnPressed: if (enabled) clicked()
    Keys.onEnterPressed: if (enabled) clicked()
    contentItem: Text {
        text: b.text
        font: b.font
        color: !b.enabled ? "#6b7280" : b.primary ? "#111111" : "#f3f4f6"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 10
        color: !b.enabled ? "#1f2328" : b.primary ? (b.down ? Qt.darker(b.accent, 1.2) : b.accent)
                                                  : (b.down ? "#3a3f46" : b.hovered ? "#30353c" : "#262a30")
        border.width: b.activeFocus ? 3 : 1
        border.color: b.activeFocus ? "#ffffff" : "#3a3f46"
        Behavior on border.width { NumberAnimation { duration: 80 } }
    }
}
