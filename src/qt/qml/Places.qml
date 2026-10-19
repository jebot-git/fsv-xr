// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls

ThemedPanel {
    id: panel
    objectName: "placesPanel"
    readonly property int currentPlace: {
        let best = -1, length = -1
        for (let i = 0; i < files.places.length; ++i) {
            const path = files.places[i].path
            if ((files.root === path || files.root.startsWith(path.endsWith("/") ? path : path + "/")) && path.length > length) {
                best = i; length = path.length
            }
        }
        return best
    }
    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: files.places
        currentIndex: panel.currentPlace
        ScrollBar.vertical: ScrollBar {}
        delegate: Column {
            required property var modelData
            required property int index
            width: list.width
            Label {
                palette: Theme.controlPalette(Theme.paletteRevision)
                text: modelData.group
                color: Theme.muted
                font.bold: true
                leftPadding: 12; topPadding: 14; bottomPadding: 6
                visible: index === 0 || files.places[index - 1].group !== modelData.group
                height: visible ? implicitHeight : 0
            }
            ItemDelegate {
                objectName: "place" + index
                width: list.width
                height: 40
                palette: Theme.controlPalette(Theme.paletteRevision)
                text: modelData.name
                icon.name: modelData.icon
                icon.width: 20; icon.height: 20
                highlighted: panel.currentPlace === index
                enabled: !files.operating
                onClicked: files.openPlace(modelData.path)
                ToolTip.visible: hovered
                ToolTip.text: modelData.path
                ToolTip.delay: 700
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.MiddleButton
                    onClicked: files.openPlace(modelData.path, true)
                }
            }
        }
    }
}
