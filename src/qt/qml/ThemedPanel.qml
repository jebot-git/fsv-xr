// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls

Pane {
    id: panel
    padding: 0
    property color color: Theme.surface
    property color borderColor: "transparent"
    property real borderWidth: 0
    property real radius: 0
    palette: Theme.controlPalette(Theme.paletteRevision)
    background: Rectangle {
        color: panel.color
        radius: panel.radius
        border.color: panel.borderColor
        border.width: panel.borderWidth
    }
}
