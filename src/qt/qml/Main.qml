// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
ApplicationWindow { palette: Theme.controlPalette(Theme.paletteRevision);
    width: 1280; height: 820
    visible: true
    title: "fsvr — " + files.root
    property alias zoom: browser.zoom
    property alias panX: browser.panX
    property alias panY: browser.panY
    property alias panZ: browser.panZ
    property alias yaw: browser.yaw
    property alias pitch: browser.pitch
    Browser { id: browser; anchors.fill: parent }
}
