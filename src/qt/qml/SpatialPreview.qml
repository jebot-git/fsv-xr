// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick3D

Node {
    id: popup
    property bool dismissed: false
    signal requestActions()
    visible: files.selected >= 0 && !dismissed
    scale: Qt.vector3d(.08, .08, .08)
    function reopen() { dismissed = false }
    Connections {
        target: files
        function onSelectedChanged() { popup.dismissed = false }
    }
    ThemedPanel {
        width: 360; height: 500
        color: Theme.surface; radius: 8
        borderColor: Theme.accent; borderWidth: 2
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 6
            RowLayout {
                Label { palette: Theme.controlPalette(Theme.paletteRevision); text: "File preview"; color: Theme.text; Layout.fillWidth: true; leftPadding: 10 }
                Button { palette: Theme.controlPalette(Theme.paletteRevision); text: "Close"; onClicked: popup.dismissed = true }
            }
            Inspector {
                Layout.fillWidth: true; Layout.fillHeight: true
                onRequestAction: popup.requestActions()
            }
        }
    }
}
