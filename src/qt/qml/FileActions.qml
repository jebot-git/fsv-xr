// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: actions
    property string editAction: ""
    function confirmRemoval() { confirmDelete.open() }
    function show(x, y) { menu.x = x; menu.y = y; menu.open() }
    function edit(action) {
        editAction = action
        field.text = action === "rename" ? files.selection.name : ""
        editDialog.open()
        field.forceActiveFocus()
        field.selectAll()
    }
    Menu { palette: Theme.controlPalette(Theme.paletteRevision);
        id: menu
        objectName: "contextMenu"
        popupType: Popup.Item
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open"; enabled: files.selected >= 0; onTriggered: files.open(files.selected) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open in New Tab"; enabled: files.selected >= 0 && files.selection.directory; onTriggered: files.newTab(files.selection.path) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open With…"; enabled: files.selected >= 0 && !files.selection.directory; onTriggered: actions.edit("openWith") }
        MenuSeparator {}
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Cut"; enabled: files.selected >= 0; onTriggered: files.clipboard(true) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Copy"; enabled: files.selected >= 0; onTriggered: files.clipboard(false) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Paste"; enabled: files.canPaste(); onTriggered: files.paste() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Copy Location"; enabled: files.selected >= 0; onTriggered: files.copyLocation() }
        MenuSeparator {}
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Rename…"; enabled: files.selected >= 0; onTriggered: actions.edit("rename") }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Move to Trash"; enabled: files.selected >= 0; onTriggered: files.remove(false) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Delete Permanently…"; enabled: files.selected >= 0; onTriggered: confirmDelete.open() }
        MenuSeparator {}
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Create Folder…"; onTriggered: actions.edit("mkdir") }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open Terminal Here"; onTriggered: files.terminal() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Properties"; enabled: files.selected >= 0; onTriggered: properties.open() }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: editDialog
        popupType: Popup.Item
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 30)
        modal: true
        title: actions.editAction === "rename" ? "Rename" : actions.editAction === "mkdir" ? "Create Folder" : "Open With"
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            width: parent.width
            Label { palette: Theme.controlPalette(Theme.paletteRevision); text: actions.editAction === "openWith" ? "Executable name or path (one file argument is appended):" : "Name:"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            TextField { palette: Theme.controlPalette(Theme.paletteRevision); id: field; objectName: "nameField"; Layout.fillWidth: true; onAccepted: editDialog.accept() }
        }
        onAccepted: {
            if (actions.editAction === "rename") files.rename(field.text)
            else if (actions.editAction === "mkdir") files.newFolder(field.text)
            else files.openWith(field.text)
        }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: confirmDelete
        popupType: Popup.Item
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 30)
        modal: true
        title: "Delete permanently?"
        standardButtons: Dialog.Yes | Dialog.No
        Label { palette: Theme.controlPalette(Theme.paletteRevision); width: parent.width; text: "Delete “" + (files.selection.name || "") + "” and its contents? This cannot be undone."; wrapMode: Text.WordWrap }
        onAccepted: files.remove(true)
    }
    Popup { palette: Theme.controlPalette(Theme.paletteRevision);
        id: properties
        popupType: Popup.Item
        anchors.centerIn: parent
        padding: 0
        width: Math.min(360, parent.width - 20); height: Math.min(570, parent.height - 20)
        Inspector { anchors.fill: parent; onRequestAction: properties.close() }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: errorDialog
        popupType: Popup.Item
        property string message: ""
        anchors.centerIn: parent
        width: Math.min(460, parent.width - 30)
        title: "fsv-xr"
        modal: true
        standardButtons: Dialog.Ok
        Label { palette: Theme.controlPalette(Theme.paletteRevision); width: parent.width; text: errorDialog.message; wrapMode: Text.WrapAnywhere }
    }
    Connections {
        target: files
        function onError(message) { errorDialog.message = message; errorDialog.open() }
    }
}
