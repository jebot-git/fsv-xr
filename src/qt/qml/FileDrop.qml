// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick

DropArea {
    id: area
    property string directory: ""
    function validate(drag) { drag.accepted = drag.hasUrls && files.canDrop(drag.urls, directory) }
    onEntered: drag => validate(drag)
    onPositionChanged: drag => validate(drag)
    onDropped: drop => {
        if (files.dropFiles(drop.urls, directory, drop.proposedAction)) drop.acceptProposedAction()
        else drop.accepted = false
    }
    Rectangle {
        anchors.fill: parent
        visible: area.containsDrag
        color: "transparent"
        border.width: 2; border.color: Theme.colors.highlight
    }
}
