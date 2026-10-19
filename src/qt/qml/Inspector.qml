// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    width: 320
    height: 570
    color: Theme.surface
    radius: 8
    signal requestAction(string action)
    Flickable {
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ColumnLayout {
            id: column
            width: parent.width
            spacing: 10
            Label { palette: Theme.controlPalette(Theme.paletteRevision); text: "FILE INSPECTOR"; color: Theme.muted; font.pixelSize: 12; font.letterSpacing: 2 }
            Image {
                Layout.fillWidth: true
                Layout.preferredHeight: 170
                source: files.selection.thumbnail || ""
                sourceSize: Qt.size(512, 512)
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }
            Label { palette: Theme.controlPalette(Theme.paletteRevision);
                text: files.selection.name || "Select a file"
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                font.pixelSize: 20; font.bold: true; color: Theme.text
            }
            Label { palette: Theme.controlPalette(Theme.paletteRevision);
                text: files.selected < 0 ? "Explore the directory in three dimensions." :
                    (files.selection.mime + "\n" + (files.inTrash && files.selection.directory ? "Folder" : Number(files.selection.bytes).toLocaleString() + " bytes") + "\n" +
                     "Modified: " + files.selection.modified + "\nOwner: " + files.selection.owner +
                     (files.selection.symlink ? "\nSymbolic link" : ""))
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.text
            }
            Label { palette: Theme.controlPalette(Theme.paletteRevision);
                text: files.inTrash && files.selected >= 0 ? "Original location: " + (files.selection.originalPath || "Unavailable — cannot restore automatically") + "\nDeleted: " + (files.selection.deleted || "Unknown") : files.selection.path || files.root
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted; font.pixelSize: 11
            }
            RowLayout {
                Button { palette: Theme.controlPalette(Theme.paletteRevision); objectName: "inspectorOpen"; text: files.inTrash ? "Restore" : "Open"; enabled: files.selected >= 0 && !files.busy && (!files.inTrash || files.selection.canRestore); onClicked: files.open(files.selected) }
                Button { palette: Theme.controlPalette(Theme.paletteRevision); text: "Actions…"; enabled: files.selected >= 0 && !files.busy; onClicked: panel.requestAction("menu") }
            }
        }
    }
}
