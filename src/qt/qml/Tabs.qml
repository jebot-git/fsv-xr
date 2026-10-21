// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ThemedPanel {
    id: strip
    implicitHeight: 40
    property bool shortcutsEnabled: true
    function revealCurrent() {
        const tab = repeater.itemAt(files.currentTab)
        if (!tab) return
        if (tab.x < flick.contentX) flick.contentX = tab.x
        else if (tab.x + tab.width > flick.contentX + flick.width)
            flick.contentX = tab.x + tab.width - flick.width
    }
    Flickable {
        id: flick
        anchors { left: parent.left; right: add.left; top: parent.top; bottom: parent.bottom }
        contentWidth: Math.max(row.width, width)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        MouseArea {
            objectName: "emptyTabBar"
            x: row.width
            width: Math.max(0, flick.width - row.width)
            height: flick.height
            acceptedButtons: Qt.LeftButton
            enabled: !files.operating
            onDoubleClicked: files.newTab()
        }
        Row {
            id: row
            height: flick.height
            spacing: 2
            Repeater {
                id: repeater
                model: files.tabs
                delegate: Button { font: Theme.font;
                    required property var modelData
                    required property int index
                    objectName: "directoryTab" + index
                    width: Math.min(220, Math.max(120, flick.width / Math.min(files.tabs.length, 4)))
                    height: strip.height
                    palette: Theme.controlPalette(Theme.paletteRevision)
                    highlighted: files.currentTab === index
                    enabled: !files.operating
                    onClicked: files.activateTab(index)
                    ToolTip.visible: hovered
                    ToolTip.text: modelData.path
                    ToolTip.delay: 700
                    contentItem: RowLayout {
                        Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering;
                            text: modelData.title
                            textFormat: Text.PlainText
                            color: files.currentTab === index ? Theme.colors.highlightedText : Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        ToolButton { font: Theme.font;
                            objectName: "closeTab" + index
                            palette: Theme.controlPalette(Theme.paletteRevision)
                            text: "×"
                            implicitWidth: 28
                            enabled: files.tabs.length > 1
                            Accessible.name: "Close tab"
                            onClicked: files.closeTab(index)
                        }
                    }
                    FileDrop { anchors.fill: parent; directory: modelData.path }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.MiddleButton
                        onClicked: files.closeTab(index)
                    }
                }
            }
        }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
    }
    ToolButton { font: Theme.font;
        id: add
        objectName: "newTabButton"
        palette: Theme.controlPalette(Theme.paletteRevision)
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 40
        text: "+"
        enabled: !files.operating
        Accessible.name: "New tab"
        onClicked: files.newTab()
        ToolTip.visible: hovered
        ToolTip.text: "New tab (Ctrl+T)"
    }
    Connections {
        target: files
        function onTabsChanged() { Qt.callLater(strip.revealCurrent) }
        function onCurrentTabChanged() { Qt.callLater(strip.revealCurrent) }
    }
    Shortcut { sequence: "Ctrl+T"; enabled: strip.shortcutsEnabled && !files.operating; onActivated: files.newTab() }
    Shortcut { sequence: "Ctrl+W"; enabled: strip.shortcutsEnabled && !files.operating; onActivated: files.closeTab(files.currentTab) }
    Shortcut { sequence: "Ctrl+Tab"; enabled: strip.shortcutsEnabled && !files.operating; onActivated: files.activateTab((files.currentTab + 1) % files.tabs.length) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; enabled: strip.shortcutsEnabled && !files.operating; onActivated: files.activateTab((files.currentTab + files.tabs.length - 1) % files.tabs.length) }
}
