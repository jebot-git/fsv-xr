// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick3D
import QtQuick3D.Xr
import Fsv 1.0

XrController {
    id: hand
    required property XrView view
    required property var interaction
    property alias gesture: gesture
    property double lastUpdate: 0
    property int previousButton: 0
    poseSpace: XrController.AimPose
    onJointDataUpdated: {
        lastUpdate = Date.now()
        gesture.update(jointPositions, isActive)
    }
    onIsActiveChanged: { if (!isActive) gesture.reset() }
    HandGesture {
        id: gesture
        onChanged: {
            if (button === 1 && hand.previousButton !== 1) hand.interaction.selectRay(hand)
            if (button === 3 && hand.previousButton !== 3) hand.interaction.contextRay(hand)
            hand.previousButton = button
        }
    }
    Timer {
        interval: 50; running: true; repeat: true
        onTriggered: { if (Date.now() - hand.lastUpdate > 200) gesture.reset() }
    }
    Model {
        source: "#Sphere"
        scale: Qt.vector3d(.018, .018, .018)
        materials: PrincipledMaterial { lighting: PrincipledMaterial.NoLighting; baseColor: gesture.button ? "#ffc66b" : "#7ad6df" }
    }
    Model {
        source: "#Cylinder"
        y: 0; z: -50
        eulerRotation.x: 90
        scale: Qt.vector3d(.002, 1, .002)
        materials: PrincipledMaterial { lighting: PrincipledMaterial.NoLighting; baseColor: "#7ad6df"; opacity: .45 }
    }
    XrVirtualMouse {
        source: hand
        view: hand.view
        enabled: hand.isActive
        leftMouseButton: gesture.button === 1
        rightMouseButton: gesture.button === 3
        middleMouseButton: gesture.button === 2
    }
}
