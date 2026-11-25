// joyride launcher: the games, installing them, preparing joyride, the
// settings, playing. Made for a pad as much as for keys and mouse: every
// page is walked with the arrows (d-pad, stick), Return (A) chooses, Escape
// (B) goes back.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qt.labs.folderlistmodel

ApplicationWindow {
    id: win
    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: "joyride"
    color: "#0f1115"
    visibility: backend.playing ? Window.Minimized : Window.AutomaticVisibility

    readonly property color accent: "#f0b429"
    readonly property color text: "#f3f4f6"
    readonly property color dim: "#9ca3af"

    function gameById(id) {
        for (var i = 0; i < backend.games.length; i++)
            if (backend.games[i].id === id) return backend.games[i]
        return null
    }

    // a city at night from above, behind everything (art/, joyride's own)
    Image {
        anchors.fill: parent
        source: "qrc:/art/background.png"
        fillMode: Image.PreserveAspectCrop
        opacity: 0.8
        asynchronous: true
    }

    header: Item {
        height: 84
        RowLayout {
            anchors { fill: parent; leftMargin: 40; rightMargin: 40 }
            Image { source: "qrc:/art/joyride-128.png"; sourceSize.width: 128; Layout.preferredWidth: 48; Layout.preferredHeight: 48; Layout.rightMargin: 12; smooth: true; mipmap: true }
            Text { text: "joyride"; color: win.accent; font.pixelSize: 40; font.bold: true }
            Text { text: stack.depth > 1 && stack.currentItem.title ? "›  " + stack.currentItem.title : ""; color: win.text; font.pixelSize: 26; Layout.leftMargin: 16 }
            Item { Layout.fillWidth: true }
            Text {
                text: (pad.connected ? "🎮  A choose · B back" : "←↑↓→ move · Enter choose · Esc back")
                color: win.dim
                font.pixelSize: 16
            }
        }
    }

    StackView {
        id: stack
        anchors.fill: parent
        anchors.margins: 32
        initialItem: homePage
        focus: true
        Keys.onEscapePressed: if (depth > 1 && !backend.busy) pop()
    }

    Connections {
        target: pad
        function onStartPressed() {
            if (stack.currentItem && stack.currentItem.startAction) stack.currentItem.startAction()
        }
    }

    // ---- home ---------------------------------------------------------------
    Component {
        id: homePage
        FocusScope {
            property string title: ""
            function startAction() {
                var g = backend.games[cards.currentIndex]
                if (g && g.installed && g.playable && backend.joyrideReady) backend.play(g.id)
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 24
                ListView {
                    id: cards
                    Layout.fillWidth: true
                    Layout.preferredHeight: 420
                    orientation: ListView.Horizontal
                    spacing: 28
                    model: backend.games
                    focus: true
                    keyNavigationEnabled: true
                    highlightFollowsCurrentItem: true
                    clip: false
                    leftMargin: 8
                    delegate: GameCard {
                        game: modelData
                        current: cards.activeFocus && ListView.isCurrentItem
                        y: 16
                        onActivated: { cards.currentIndex = index; stack.push(gamePage, { gameId: modelData.id }) }
                    }
                    Keys.onReturnPressed: stack.push(gamePage, { gameId: backend.games[currentIndex].id })
                    Keys.onEnterPressed: stack.push(gamePage, { gameId: backend.games[currentIndex].id })
                    KeyNavigation.down: settingsBtn
                }
                RowLayout {
                    spacing: 20
                    BigButton {
                        id: settingsBtn
                        text: "Settings"
                        KeyNavigation.up: cards
                        KeyNavigation.right: prepBtn
                        onClicked: stack.push(settingsPage)
                    }
                    BigButton {
                        id: prepBtn
                        text: backend.joyrideReady ? "Rebuild joyride" : "Prepare joyride"
                        primary: !backend.joyrideReady
                        enabled: gameById("gta1") && gameById("gta1").installed
                        KeyNavigation.up: cards
                        KeyNavigation.left: settingsBtn
                        onClicked: backend.prepare()
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: win.dim
                        font.pixelSize: 17
                        text: backend.joyrideReady
                              ? "joyride is ready. Choose a game and play."
                              : (gameById("gta1") && gameById("gta1").installed)
                                ? "joyride is made on this computer from GTA's own program: choose “Prepare joyride” (it takes a few minutes, once)."
                                : "Start with Grand Theft Auto: install it, then prepare joyride."
                    }
                }
            }
        }
    }

    // ---- a game ---------------------------------------------------------------
    Component {
        id: gamePage
        FocusScope {
            id: gp
            property string gameId
            property var game: gameById(gameId)
            property string title: game ? game.title : ""
            function startAction() { if (playBtn.visible && playBtn.enabled) playBtn.clicked() }
            // the first thing to do here takes the focus: Play, else Download, else the Steam copy, else a file
            function focusFirst() {
                var order = [playBtn, steamRepeater.count ? steamRepeater.itemAt(0) : null, fileBtn, removeBtn]
                for (var i = 0; i < order.length; i++)
                    if (order[i] && order[i].visible && order[i].enabled) { order[i].forceActiveFocus(); return }
            }
            Component.onCompleted: Qt.callLater(focusFirst)
            StackView.onActivated: Qt.callLater(focusFirst)
            ColumnLayout {
                anchors.fill: parent
                spacing: 18
                Text { text: gp.game ? gp.game.subtitle : ""; color: win.dim; font.pixelSize: 20 }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: win.text
                    font.pixelSize: 19
                    visible: text !== ""
                    text: !gp.game ? "" :
                          !gp.game.playable ? "joyride does not play this one yet." :
                          gp.game.installed ? "Installed in " + gp.game.playDir :
                          !gp.game.needsMet ? "Needs " + gp.game.needsTitle + " installed first." :
                          gp.gameId === "london1969" ? "Install it from your own disc: a BIN/CUE or ISO image, or the disc itself." :
                          gp.gameId === "london1961" ? "Install it from your copy of its installer, GTAL1961.EXE." :
                          "Install it from your own copy: a Steam install, an install folder or the 2002 installer."
                }
                ColumnLayout {
                    id: actions
                    spacing: 14
                    Layout.topMargin: 12
                    BigButton {
                        id: playBtn
                        text: "Play"
                        primary: true
                        visible: gp.game && gp.game.installed && gp.game.playable
                        enabled: backend.joyrideReady
                        KeyNavigation.down: steamRepeater.count ? steamRepeater.itemAt(0) : fileBtn
                        onClicked: backend.play(gp.gameId)
                    }
                    Text {
                        visible: playBtn.visible && !backend.joyrideReady
                        text: "Prepare joyride first (on the home page)."
                        color: win.dim
                        font.pixelSize: 16
                    }
                    Repeater {
                        id: steamRepeater
                        model: gp.gameId === "gta1" ? backend.steamInstalls : []
                        BigButton {
                            text: "Use the Steam copy (" + modelData + ")"
                            KeyNavigation.up: index > 0 ? steamRepeater.itemAt(index - 1) : playBtn
                            KeyNavigation.down: index + 1 < steamRepeater.count ? steamRepeater.itemAt(index + 1) : fileBtn
                            onClicked: backend.installFrom("gta1", modelData)
                        }
                    }
                    BigButton {
                        id: fileBtn
                        text: "Install from a file or folder…"
                        visible: gp.game && gp.game.playable
                        enabled: gp.game && gp.game.needsMet
                        KeyNavigation.up: steamRepeater.count ? steamRepeater.itemAt(steamRepeater.count - 1) : playBtn
                        KeyNavigation.down: removeBtn
                        onClicked: stack.push(browsePage, { gameId: gp.gameId })
                    }
                    BigButton {
                        id: removeBtn
                        text: "Remove"
                        visible: gp.game && gp.game.installed && gp.gameId !== "gta1" || (gp.gameId === "gta1" && gp.game && gp.game.installed)
                        KeyNavigation.up: fileBtn
                        onClicked: confirm.open()
                    }
                }
                Item { Layout.fillHeight: true }
            }
            Dialog {
                id: confirm
                anchors.centerIn: parent
                modal: true
                title: "Remove " + (gp.game ? gp.game.title : "") + "?"
                standardButtons: Dialog.Yes | Dialog.No
                onAccepted: backend.uninstall(gp.gameId)
                Label { text: gp.gameId === "gta1" ? "A Steam copy used where it is stays; London is removed with it." : "Its saves stay."; color: win.text }
            }
        }
    }

    // ---- choosing a file or folder ---------------------------------------------
    Component {
        id: browsePage
        FocusScope {
            id: bp
            property string gameId
            property string title: "Choose the source"
            Component.onCompleted: list.forceActiveFocus()
            FolderListModel {
                id: folder
                folder: "file://" + backend.homeDir
                showDirsFirst: true
                showDotAndDotDot: false
                showHidden: false
                nameFilters: backend.sourceFilters(bp.gameId)
                caseSensitive: false
            }
            function up() {
                var p = folder.folder.toString()
                if (p === "file:///") return
                folder.folder = folder.parentFolder
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: backend.sourceHint(bp.gameId); color: win.dim; font.pixelSize: 17 }
                Text { text: decodeURIComponent(folder.folder.toString().replace("file://", "")); color: win.accent; font.pixelSize: 18; elide: Text.ElideMiddle; Layout.fillWidth: true }
                RowLayout {
                    spacing: 14
                    BigButton { id: upBtn; text: "↑ Up a folder"; KeyNavigation.down: list; KeyNavigation.right: useBtn; onClicked: bp.up() }
                    BigButton {
                        id: useBtn
                        text: "Use this folder"
                        KeyNavigation.down: list
                        KeyNavigation.left: upBtn
                        KeyNavigation.right: mediaBtn
                        onClicked: { backend.installFrom(bp.gameId, decodeURIComponent(folder.folder.toString().replace("file://", ""))); stack.pop() }
                    }
                    BigButton { id: mediaBtn; text: "Discs and drives"; KeyNavigation.down: list; KeyNavigation.left: useBtn; onClicked: folder.folder = "file:///run/media" }
                }
                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: folder
                    focus: true
                    keyNavigationEnabled: true
                    highlightMoveDuration: 80
                    KeyNavigation.up: currentIndex <= 0 ? upBtn : null
                    Keys.onUpPressed: function(e) { if (currentIndex <= 0) upBtn.forceActiveFocus(); else decrementCurrentIndex() }
                    Keys.onLeftPressed: bp.up()
                    Keys.onBackPressed: bp.up()
                    function choose(i) {
                        if (i < 0 || i >= folder.count) return
                        if (folder.isFolder(i)) { folder.folder = folder.get(i, "fileUrl"); currentIndex = 0 }
                        else { backend.installFrom(bp.gameId, folder.get(i, "filePath")); stack.pop() }
                    }
                    Keys.onReturnPressed: choose(currentIndex)
                    Keys.onEnterPressed: choose(currentIndex)
                    Keys.onRightPressed: if (folder.isFolder(currentIndex)) choose(currentIndex)
                    highlight: Rectangle { color: "#2a2f37"; radius: 8; border.color: list.activeFocus ? "#ffffff" : "transparent"; border.width: 2 }
                    delegate: Item {
                        width: ListView.view.width
                        height: 48
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 16
                            text: (fileIsDir ? "📁  " : "📄  ") + fileName
                            color: fileIsDir ? win.text : win.accent
                            font.pixelSize: 20
                        }
                        MouseArea { anchors.fill: parent; onClicked: list.currentIndex = index; onDoubleClicked: list.choose(index) }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }
    }

    // ---- settings ------------------------------------------------------------------
    Component {
        id: settingsPage
        FocusScope {
            id: sp
            property string title: "Settings"
            property var model: backend.settingsModel()
            Component.onCompleted: rows.forceActiveFocus()
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                Text { text: "Shared with the game's own settings (F1, or L3+R3, in play). ← → change."; color: win.dim; font.pixelSize: 17 }
                ListView {
                    id: rows
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: sp.model
                    focus: true
                    keyNavigationEnabled: true
                    spacing: 6
                    clip: true
                    function step(i, d) {
                        var item = sp.model[i], opts = item.options, at = 0
                        for (var k = 0; k < opts.length; k++) if (opts[k].value === item.value) at = k
                        at = (at + d + opts.length) % opts.length
                        backend.setSetting(item.key, opts[at].value)
                        var keep = currentIndex
                        sp.model = backend.settingsModel()
                        currentIndex = keep
                    }
                    Keys.onLeftPressed: step(currentIndex, -1)
                    Keys.onRightPressed: step(currentIndex, 1)
                    Keys.onReturnPressed: step(currentIndex, 1)
                    Keys.onEnterPressed: step(currentIndex, 1)
                    delegate: Item {
                        id: setting
                        width: ListView.view.width
                        // the first of a group has the group's name over it
                        property bool first: index === 0 || sp.model[index - 1].group !== modelData.group
                        height: 60 + (first ? 44 : 0)
                        property var opts: modelData.options
                        function labelOf(v) { for (var k = 0; k < opts.length; k++) if (opts[k].value === v) return opts[k].label; return v }
                        Text { visible: first; y: 10; x: 4; text: modelData.group; color: win.dim; font.pixelSize: 18; font.bold: true; font.capitalization: Font.AllUppercase; font.letterSpacing: 2 }
                        Item {
                        id: row
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 60
                        Rectangle {                      // the one chosen (the group's name stays outside it)
                            anchors.fill: parent
                            radius: 10
                            visible: setting.ListView.isCurrentItem
                            color: "#262a30"
                            border.color: rows.activeFocus ? "#ffffff" : "transparent"
                            border.width: 2
                        }
                        Text { anchors.verticalCenter: parent.verticalCenter; x: 20; text: modelData.label; color: win.text; font.pixelSize: 22 }
                        Row {
                            anchors { right: parent.right; rightMargin: 20; verticalCenter: parent.verticalCenter }
                            spacing: 14
                            Text { text: "◀"; color: win.dim; font.pixelSize: 20; MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: { rows.currentIndex = index; rows.step(index, -1) } } }
                            Text { text: labelOf(modelData.value); color: win.accent; font.pixelSize: 22; width: 420; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
                            Text { text: "▶"; color: win.dim; font.pixelSize: 20; MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: { rows.currentIndex = index; rows.step(index, 1) } } }
                        }
                        }
                        MouseArea { anchors.fill: parent; z: -1; onClicked: rows.currentIndex = index }
                    }
                }
            }
        }
    }

    // ---- a task under way, or one that failed ------------------------------------------
    Rectangle {
        id: overlay
        anchors.fill: parent
        color: "#e60f1115"
        visible: backend.busy || backend.lastError !== ""
        z: 10
        onVisibleChanged: if (visible) closeBtn.forceActiveFocus(); else stack.currentItem && stack.currentItem.forceActiveFocus()
        MouseArea { anchors.fill: parent }       // nothing behind it is pressed meanwhile
        ColumnLayout {
            anchors { fill: parent; margins: 60 }
            spacing: 18
            Text { text: backend.busy ? backend.task : "Something went wrong"; color: backend.busy ? win.text : "#f87171"; font.pixelSize: 30; font.bold: true }
            Text { visible: !backend.busy; text: backend.lastError; color: win.text; font.pixelSize: 20; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ProgressBar {
                visible: backend.busy
                Layout.fillWidth: true
                value: backend.progress
                background: Rectangle { implicitHeight: 14; radius: 7; color: "#262a30" }
                contentItem: Item {
                    implicitHeight: 14
                    Rectangle { width: parent.width * backend.progress; height: parent.height; radius: 7; color: win.accent }
                }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                TextArea {
                    id: logView
                    readOnly: true
                    text: backend.log
                    color: win.dim
                    font.family: "monospace"
                    font.pixelSize: 14
                    wrapMode: TextArea.Wrap
                    background: Rectangle { color: "#14171b"; radius: 8 }
                    onTextChanged: cursorPosition = length
                    focusPolicy: Qt.NoFocus
                }
            }
            BigButton {
                id: closeBtn
                text: backend.busy ? "Cancel" : "Close"
                onClicked: backend.busy ? backend.cancel() : backend.clearError()
                Keys.onEscapePressed: clicked()
            }
        }
    }

    Connections {
        target: backend
        function onFinished(ok, message) {
            // a finished install goes back to its game's page, now with Play
            if (ok && stack.currentItem && stack.currentItem.gameId === undefined && stack.depth > 1) stack.pop()
        }
    }
}
