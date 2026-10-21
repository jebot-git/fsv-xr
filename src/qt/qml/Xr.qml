// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick3D
import QtQuick3D.Xr

XrView {
    id: view
    referenceSpace: XrView.ReferenceSpaceLocalFloor
    onInitializeFailed: message => { console.error("OpenXR initialization failed: " + message); Qt.exit(1) }
    environment: SceneEnvironment { backgroundMode: SceneEnvironment.Color; clearColor: "transparent" }
    passthroughEnabled: passthroughSupported
    property real zoom: .65
    property real panX: 0
    property real panY: 0
    property real panZ: 0
    property real yaw: 0
    property real pitch: 0
    property bool viewReady: false
    property bool restoringView: false
    function storeView() {
        if (viewReady && !restoringView) files.viewState = {zoom: zoom, panX: panX, panY: panY, panZ: panZ, yaw: yaw, pitch: pitch}
    }
    function restoreView() {
        const state = files.viewState
        restoringView = true
        zoom = state.zoom ?? .65; panX = state.panX ?? 0; panY = state.panY ?? 0; panZ = state.panZ ?? 0
        yaw = state.yaw ?? 0; pitch = state.pitch ?? 0
        restoringView = false
    }
    onZoomChanged: storeView()
    onPanXChanged: storeView()
    onPanYChanged: storeView()
    onPanZChanged: storeView()
    onYawChanged: storeView()
    onPitchChanged: storeView()
    Component.onCompleted: { restoreView(); viewReady = true }
    Connections { target: files; function onViewStateChanged() { if (view.viewReady) view.restoreView() } }
    Connections { target: files; function onPropertiesRequested() { inspector.reveal() } }
    function changeZoom(delta) {
        const target = Math.max(.1, Math.min(3, zoom * Math.exp(delta)))
        if (target === zoom) return
        const selection = files.selection
        const position = files.selected < 0 ? null : world.positionForZoom(world.selectionTop(selection), target / zoom)
        restoringView = true
        if (position) { panX = position.x; panY = position.y - 85; panZ = position.z + 140 }
        zoom = target
        restoringView = false
        storeView()
    }
    property real previousSeparation: 0
    property vector3d previousPosition: Qt.vector3d(0, 0, 0)
    property bool wasDragging: false
    property int dragHand: -1
    function hit(hand) { return view.rayPick(hand.scenePosition, hand.forward) }
    function selectRay(hand) {
        const result = hit(hand)
        if (result.objectHit && result.objectHit.entryIndex !== undefined) {
            files.selected = result.objectHit.entryIndex
            inspector.reveal()
        }
    }
    function contextRay(hand) {
        selectRay(hand)
        controls.show(8, 40)
    }
    xrOrigin: origin
    XrOrigin {
        id: origin
        XrHand { id: left; controller: XrController.LeftController; view: view; interaction: view }
        XrHand { id: right; controller: XrController.RightController; view: view; interaction: view }
    }
    World {
        id: world
        showBase: false
        yaw: view.yaw; pitch: view.pitch
        position: Qt.vector3d(view.panX, 85 + view.panY, -140 + view.panZ)
        scale: Qt.vector3d(view.zoom, view.zoom, view.zoom)
        texturePixelsPerUnit: 16 * view.zoom
    }
    // An always-present control panel, independent of file selection.
    Node {
        position: Qt.vector3d(-100, 150, -140)
        eulerRotation.y: 20
        scale: Qt.vector3d(.12, .12, .12)
        Places { width: 250; height: 720 }
    }
    Node {
        position: Qt.vector3d(100, 150, -140)
        eulerRotation.y: -20
        scale: Qt.vector3d(.12, .12, .12)
        ThemedPanel {
            width: 360; height: 720; color: Theme.surface; radius: 8
            focus: true
            Keys.enabled: activeFocus
            Keys.onLeftPressed: view.yaw -= 5
            Keys.onRightPressed: view.yaw += 5
            Keys.onUpPressed: view.pitch = Math.max(-30, view.pitch - 5)
            Keys.onDownPressed: view.pitch = Math.min(45, view.pitch + 5)
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 10
                Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision); text: "fsvr · OpenXR / Vulkan"; color: Theme.text; }
                Tabs { Layout.fillWidth: true; Layout.preferredHeight: 40 }
                Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision); text: files.root; color: Theme.muted; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                RowLayout {
                    Button { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); text: "Up"; enabled: !files.operating; onClicked: files.up() }
                    Button { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); text: "Refresh"; enabled: !files.operating; onClicked: files.refresh() }
                    ComboBox { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); model: ["map", "tree", "disc"]; currentIndex: ["map", "tree", "disc"].indexOf(files.layout); onActivated: files.layout = currentText }
                }
                Button { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); text: "Empty Trash…"; visible: files.inTrash; enabled: !files.busy; onClicked: controls.confirmEmptyTrash() }
                Inspector { id: inspector; Layout.fillWidth: true; Layout.fillHeight: true; onRequestAction: controls.show(8, 40) }
                RowLayout {
                    Button { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); text: "− Zoom"; onClicked: view.changeZoom(-Math.log(1.2)) }
                    Button { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); text: "+ Zoom"; onClicked: view.changeZoom(Math.log(1.2)) }
                    Button { font: Theme.font; palette: Theme.controlPalette(Theme.paletteRevision); text: "Reset"; onClicked: { view.zoom = .65; view.panX = 0; view.panY = 0; view.panZ = 0; view.yaw = 0; view.pitch = 0 } }
                }
                Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision); text: "Index pinch: select / click\nMiddle pinch + move: drag view\nTwo middle pinches: scale\nRing pinch: actions"; color: Theme.muted }
                Label { font: Theme.font; renderType: Theme.dome ? Text.NativeRendering : Text.QtRendering; palette: Theme.controlPalette(Theme.paletteRevision); text: files.warning; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: Theme.text; }
            }
            FileActions { id: controls; anchors.fill: parent; onInspectorRequested: inspector.reveal() }
        }
    }
    Timer {
        interval: 16; running: true; repeat: true
        onTriggered: {
            const l = left.gesture.dragging, r = right.gesture.dragging
            if (l && r) {
                const distance = left.scenePosition.minus(right.scenePosition).length()
                if (view.previousSeparation > 1 && distance > 1)
                    view.changeZoom(Math.log(distance / view.previousSeparation))
                view.previousSeparation = distance
                view.wasDragging = false
                view.dragHand = -1
            } else if (l || r) {
                const identity = l ? 0 : 1
                const position = l ? left.scenePosition : right.scenePosition
                if (view.wasDragging && view.dragHand === identity) {
                    const delta = position.minus(view.previousPosition)
                    view.panX += delta.x
                    view.panZ += delta.z
                }
                view.previousPosition = position
                view.wasDragging = true
                view.dragHand = identity
                view.previousSeparation = 0
            } else {
                view.wasDragging = false; view.previousSeparation = 0; view.dragHand = -1
            }
        }
    }
}
