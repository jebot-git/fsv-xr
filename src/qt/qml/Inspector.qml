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
    clip: true
    signal requestAction(string action)
    function reveal() { details.contentY = 0; forceActiveFocus() }
    Flickable {
        id: details
        anchors { top: parent.top; left: parent.left; right: parent.right; bottom: buttons.top }
        anchors.margins: 16
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ColumnLayout {
            id: column
            width: parent.width
            spacing: 10
            Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision); text: "FILE INSPECTOR"; color: Theme.muted; }
            Image {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(170, panel.height * .3)
                visible: files.selected >= 0
                source: files.selection.thumbnail ? files.selection.thumbnail + "/font/" + Theme.font.family + "-" + Theme.font.styleName + "-" + Theme.font.pixelSize : ""
                sourceSize: Qt.size(512, 512)
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }
            Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision);
                text: files.selection.name || "Select a file"
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                  color: Theme.text
            }
            Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision);
                text: files.selected < 0 ? "Explore the directory in three dimensions." :
                    (files.selection.mime + "\n" + (files.inTrash && files.selection.directory ? "Folder" : Number(files.selection.bytes).toLocaleString() + " bytes") + "\n" +
                     "Modified: " + (files.selection.modified || "Unknown") + (files.inArchive ? "" : "\nOwner: " + files.selection.owner) +
                     (files.selection.symlink ? (files.inArchive ? "\nLink or special file (opening disabled)" : "\nSymbolic link") : ""))
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.text
            }
            Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision);
                text: files.inTrash && files.selected >= 0 ? "Original location: " + (files.selection.originalPath || "Unavailable — cannot restore automatically") + "\nDeleted: " + (files.selection.deleted || "Unknown") : files.selection.path || files.root
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted;
            }
        }
    }
    RowLayout {
        id: buttons
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 12 }
        spacing: 8
        Button { font: Theme.font;
            palette: Theme.controlPalette(Theme.paletteRevision)
            objectName: "inspectorOpen"
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
            leftPadding: 8; rightPadding: 8
            text: files.inTrash ? "Restore" : "Open"
            enabled: files.selected >= 0 && !!files.selection.canOpen && !files.busy && (!files.inTrash || files.selection.canRestore)
            onClicked: files.open(files.selected)
        }
        Button { font: Theme.font;
            palette: Theme.controlPalette(Theme.paletteRevision)
            objectName: "inspectorActions"
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
            leftPadding: 8; rightPadding: 8
            text: "Actions…"
            enabled: files.selected >= 0 && !files.busy
            onClicked: panel.requestAction("menu")
        }
    }
}
