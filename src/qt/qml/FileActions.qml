// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: actions
    property string editAction: ""
    property string removalPath: ""
    function confirmRemoval() { if (files.inArchive) return; removalPath = files.selection.path || ""; confirmDelete.open() }
    function confirmEmptyTrash() { if (files.inTrash) emptyTrashDialog.open() }
    signal inspectorRequested()
    function showProperties() { inspectorRequested() }
    function show(x, y) { menu.x = x; menu.y = y; menu.open() }
    function edit(action) {
        if (files.inTrash || (files.inArchive && action !== "openWith")) return
        editAction = action
        field.text = action === "rename" ? files.selection.name : action === "compress" ? files.selection.name + ".zip" : ""
        editDialog.open()
        field.forceActiveFocus()
        field.selectAll()
    }
    Menu { palette: Theme.controlPalette(Theme.paletteRevision);
        id: menu
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Restore"; visible: files.inTrash; enabled: files.selected >= 0 && files.selection.canRestore && !files.busy; onTriggered: files.restore() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Empty Trash…"; visible: files.inTrash; enabled: !files.busy; onTriggered: actions.confirmEmptyTrash() }
        objectName: "contextMenu"
        popupType: Popup.Item
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open"; visible: !files.inTrash; enabled: files.selected >= 0 && !!files.selection.canOpen && !files.busy; onTriggered: files.open(files.selected) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open in New Tab"; visible: !files.inTrash; enabled: files.selected >= 0 && (files.selection.directory || files.selection.archive); onTriggered: files.newTab(files.selection.path) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Browse Archive"; visible: !files.inTrash && !files.inArchive && !!files.selection.archive; enabled: !files.busy; onTriggered: files.browseArchive() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Extract Archive"; visible: !files.inTrash && !files.inArchive && !!files.selection.archive; enabled: !files.busy; onTriggered: files.extractArchive() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Compress…"; visible: !files.inTrash && !files.inArchive; enabled: files.selected >= 0 && !files.busy; onTriggered: actions.edit("compress") }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open With…"; visible: !files.inTrash; enabled: files.selected >= 0 && !files.selection.directory && !!files.selection.canOpen && !files.busy; onTriggered: actions.edit("openWith") }
        MenuSeparator {}
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Cut"; visible: !files.inTrash && !files.inArchive; enabled: files.selected >= 0; onTriggered: files.clipboard(true) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Copy"; visible: !files.inTrash && !files.inArchive; enabled: files.selected >= 0; onTriggered: files.clipboard(false) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Paste"; visible: !files.inTrash && !files.inArchive; enabled: files.canPaste(); onTriggered: files.paste() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Copy Location"; enabled: files.selected >= 0; onTriggered: files.copyLocation() }
        MenuSeparator {}
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Rename…"; visible: !files.inTrash && !files.inArchive; enabled: files.selected >= 0; onTriggered: actions.edit("rename") }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Move to Trash"; visible: !files.inTrash && !files.inArchive; enabled: files.selected >= 0; onTriggered: files.remove(false) }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Delete Permanently…"; visible: !files.inArchive; enabled: files.selected >= 0; onTriggered: actions.confirmRemoval() }
        MenuSeparator {}
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Create Folder…"; visible: !files.inTrash && !files.inArchive; onTriggered: actions.edit("mkdir") }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Open Terminal Here"; visible: !files.inTrash && !files.inArchive; onTriggered: files.terminal() }
        MenuItem { palette: Theme.controlPalette(Theme.paletteRevision); text: "Properties"; enabled: files.selected >= 0; onTriggered: actions.showProperties() }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: editDialog
        objectName: "editDialog"
        popupType: Popup.Item
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 30)
        modal: true
        title: actions.editAction === "rename" ? "Rename" : actions.editAction === "mkdir" ? "Create Folder" : actions.editAction === "compress" ? "Compress" : "Open With"
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            width: parent.width
            Label { palette: Theme.controlPalette(Theme.paletteRevision); text: actions.editAction === "openWith" ? "Executable name or path (one file argument is appended):" : actions.editAction === "compress" ? "Archive name (.zip or .tar.gz):" : "Name:"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            TextField { palette: Theme.controlPalette(Theme.paletteRevision); id: field; objectName: "nameField"; Layout.fillWidth: true; onAccepted: editDialog.accept() }
        }
        onAccepted: {
            if (actions.editAction === "rename") files.rename(field.text)
            else if (actions.editAction === "mkdir") files.newFolder(field.text)
            else if (actions.editAction === "compress") files.compress(field.text)
            else files.openWith(field.text)
        }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: confirmDelete
        objectName: "confirmDeleteDialog"
        popupType: Popup.Item
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 30)
        modal: true
        title: "Delete permanently?"
        standardButtons: Dialog.Yes | Dialog.No
        Label { palette: Theme.controlPalette(Theme.paletteRevision); width: parent.width; text: "Delete “" + (files.selection.name || "") + "” and its contents? This cannot be undone."; wrapMode: Text.WordWrap }
        onAccepted: { if (files.selection.path === actions.removalPath) files.remove(true) }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: emptyTrashDialog
        objectName: "emptyTrashDialog"
        popupType: Popup.Item
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 30)
        modal: true
        title: "Empty Trash?"
        standardButtons: Dialog.Yes | Dialog.No
        Label { palette: Theme.controlPalette(Theme.paletteRevision); width: parent.width; text: "Permanently delete all items in Trash on all currently available volumes? This cannot be undone."; wrapMode: Text.WordWrap }
        onAccepted: { if (files.inTrash) files.emptyTrash() }
    }
    Dialog { palette: Theme.controlPalette(Theme.paletteRevision);
        id: errorDialog
        popupType: Popup.Item
        property string message: ""
        anchors.centerIn: parent
        width: Math.min(460, parent.width - 30)
        title: "fsvr"
        modal: true
        standardButtons: Dialog.Ok
        Label { palette: Theme.controlPalette(Theme.paletteRevision); width: parent.width; text: errorDialog.message; wrapMode: Text.WrapAnywhere }
    }
    Connections {
        target: files
        function onError(message) { errorDialog.message = message; errorDialog.open() }
    }
}
