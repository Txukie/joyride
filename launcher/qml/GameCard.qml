// One game on the home page.
import QtQuick

Rectangle {
    id: card
    property var game
    property bool current: false
    signal activated()
    width: 300
    height: 380
    radius: 16
    color: "#1b1e23"
    border.width: current ? 4 : 1
    border.color: current ? "#f0b429" : "#33373d"
    scale: current ? 1.04 : 1.0
    opacity: game && !game.playable ? 0.55 : 1.0
    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }

    // a coloured band standing for the box art (none is shipped)
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
        height: 200
        radius: 10
        gradient: Gradient {
            GradientStop { position: 0; color: card.game && card.game.id.indexOf("london") === 0 ? "#1e3a8a" : card.game && card.game.id === "gta2" ? "#4c1d95" : "#7c2d12" }
            GradientStop { position: 1; color: "#0f1115" }
        }
        Text {
            anchors.centerIn: parent
            text: !card.game ? "" : card.game.id === "gta1" ? "GTA" : card.game.id === "london1969" ? "1969" : card.game.id === "london1961" ? "1961" : "GTA 2"
            font.pixelSize: 64
            font.bold: true
            color: "#f3f4f6"
            style: Text.Raised
            styleColor: "#000000"
        }
    }
    Column {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 18 }
        spacing: 6
        Text { width: parent.width; text: card.game ? card.game.title : ""; color: "#f9fafb"; font.pixelSize: 24; font.bold: true; wrapMode: Text.WordWrap }
        Text { width: parent.width; text: card.game ? card.game.subtitle : ""; color: "#9ca3af"; font.pixelSize: 16; wrapMode: Text.WordWrap }
        Text {
            text: !card.game ? "" : !card.game.playable ? "Not yet playable" : card.game.installed ? "● Installed" : "Not installed"
            color: card.game && card.game.installed ? "#34d399" : "#9ca3af"
            font.pixelSize: 16
        }
    }
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onEntered: card.ListView.view && (card.ListView.view.currentIndex = index)
        onClicked: card.activated()
    }
}
