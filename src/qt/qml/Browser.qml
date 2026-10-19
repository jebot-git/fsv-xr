// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick3D

ThemedPanel {
    id: window
    width: 1280; height: 820
    color: spatialPopups ? "transparent" : Theme.sceneBackground
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    property real panZ: 0
    property real yaw: 0
    property real pitch: 0
    property bool spatialPopups: false
    property bool viewReady: false
    property bool restoringView: false
    function storeView() {
        if (viewReady && !restoringView) files.viewState = {zoom: zoom, panX: panX, panY: panY, panZ: panZ, yaw: yaw, pitch: pitch}
    }
    function restoreView() {
        const state = files.viewState
        restoringView = true
        zoom = state.zoom ?? 1; panX = state.panX ?? 0; panY = state.panY ?? 0; panZ = state.panZ ?? 0
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
    Connections { target: files; function onViewStateChanged() { if (window.viewReady) window.restoreView() } }
    signal previewRequested(point anchor)
    signal contextMenuRequested(point anchor)
    function showActionsAt(x, y) {
        if (spatialPopups) contextMenuRequested(Qt.point(x, y))
        else actions.show(x, y)
    }
    function previewSelection() {
        if (!spatialPopups || files.selected < 0) return
        const center = files.selection.center, extent = files.selection.extent
        const top = world.mapPositionToScene(Qt.vector3d(center.x, center.y + extent.y / 2 + .3, center.z))
        const projected = view.mapFrom3DScene(top)
        if (projected.z > 0) previewRequested(view.mapToItem(window, projected.x, projected.y))
    }
    function changeZoom(delta) {
        const target = Math.max(.15, Math.min(8, zoom * Math.exp(delta)))
        if (target === zoom) return
        const selection = files.selection
        const position = files.selected < 0 ? null : world.positionForZoom(
            Qt.vector3d(selection.center.x, selection.center.y + selection.extent.y / 2 + .3, selection.center.z), target / zoom)
        restoringView = true
        if (position) { panX = position.x; panY = position.y; panZ = position.z }
        zoom = target
        restoringView = false
        storeView()
    }
    function rotateView(horizontal, vertical) { yaw += horizontal; pitch = Math.max(-30, Math.min(45, pitch + vertical)) }
    readonly property bool active: Window.window ? Window.window.active : false
    ToolBar { palette: Theme.controlPalette(Theme.paletteRevision);
        id: toolbar
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        RowLayout {
            anchors.fill: parent; anchors.margins: 5
            Label { palette: Theme.controlPalette(Theme.paletteRevision); text: " fsv-xr "; font.bold: true; font.pixelSize: 22 }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "↑ Up"; enabled: !files.operating; onClicked: files.up() }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "↻"; enabled: !files.operating; onClicked: files.refresh() }
            TextField { palette: Theme.controlPalette(Theme.paletteRevision); id: location; objectName: "locationField"; Layout.fillWidth: true; text: files.root; onAccepted: files.navigate(text) }
            ComboBox { palette: Theme.controlPalette(Theme.paletteRevision);
                model: ["Map", "Tree", "Disc"]
                currentIndex: ["map", "tree", "disc"].indexOf(files.layout)
                onActivated: files.layout = ["map", "tree", "disc"][currentIndex]
            }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "Reset View"; onClicked: { window.zoom = 1; window.panX = 0; window.panY = 0; window.panZ = 0; window.yaw = 0; window.pitch = 0 } }
        }
    }
    Tabs {
        id: tabs
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom }
        height: implicitHeight
    }
    RowLayout {
        anchors.top: tabs.bottom; anchors.bottom: status.top
        anchors.left: parent.left; anchors.right: parent.right; spacing: 0
        Places {
            Layout.preferredWidth: Math.min(200, window.width * .2)
            Layout.fillHeight: true
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            View3D {
                id: view
                objectName: "filesystemView"
                anchors.fill: parent
                environment: SceneEnvironment { backgroundMode: SceneEnvironment.Color; clearColor: window.spatialPopups ? "transparent" : Theme.sceneBackground; antialiasingMode: SceneEnvironment.MSAA }
                PerspectiveCamera {
                    id: camera
                    position: Qt.vector3d(0, 165, 200)
                    eulerRotation.x: -40
                    clipNear: .1; clipFar: 5000
                }
                camera: camera
                World { id: world; zoom: window.zoom; panX: window.panX; panY: window.panY; panZ: window.panZ; yaw: window.yaw; pitch: window.pitch; showBase: !window.spatialPopups }
            }
            MouseArea {
                id: mouse
                objectName: "navigationMouse"
                anchors.fill: parent
                focus: true
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                property point previous: Qt.point(0, 0)
                property point start: Qt.point(0, 0)
                property bool scrollMode: false
                property bool scrollWasActive: false
                property bool rotating: false
                onPressed: event => {
                    scrollWasActive = scrollMode
                    scrollMode = false
                    previous = Qt.point(event.x, event.y)
                    start = previous
                    forceActiveFocus()
                    rotating = false
                    if (event.button !== Qt.MiddleButton) {
                        const hit = view.pick(event.x, event.y)
                        files.selected = hit.objectHit ? hit.objectHit.entryIndex : -1
                        // Latch at press time: crossing an object during a
                        // background drag must not interrupt rotation.
                        rotating = event.button === Qt.LeftButton
                            && (!hit.objectHit || !!(event.modifiers & Qt.AltModifier))
                    }
                    if (event.button === Qt.RightButton) {
                        const point = mouse.mapToItem(window, event.x, event.y)
                        window.showActionsAt(point.x, point.y)
                    }
                }
                onPositionChanged: event => {
                    if (pressedButtons & Qt.MiddleButton) {
                        const dx = event.x - previous.x, dy = event.y - previous.y
                        // MMB drag pans. Shift+MMB converts vertical motion into scroll/zoom.
                        if (event.modifiers & Qt.ShiftModifier) window.changeZoom(-dy * .012)
                        else { window.panX += dx * .24; window.panZ += dy * .24 }
                    } else if (pressedButtons & Qt.LeftButton && rotating) {
                        window.rotateView((event.x - previous.x) * .4, (event.y - previous.y) * .3)
                    }
                    if (scrollMode && !pressedButtons) window.changeZoom((previous.y - event.y) * .012)
                    previous = Qt.point(event.x, event.y)
                }
                onReleased: event => {
                    rotating = false
                    if (event.button === Qt.LeftButton && !(event.modifiers & Qt.AltModifier) && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) < 5)
                        window.previewSelection()
                    // A short MMB click enables scroll-button zoom, usable with DomeD's
                    // middle/thumb pinch without needing a keyboard modifier.
                    if (!scrollWasActive && event.button === Qt.MiddleButton && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) < 5) {
                        scrollMode = true
                    }
                }
                hoverEnabled: true
                onCanceled: { scrollMode = false; rotating = false }
                onExited: { if (!pressed) scrollMode = false }
                Connections { target: window; function onActiveChanged() { if (!window.active) { mouse.scrollMode = false } } }
                onDoubleClicked: event => { if (event.button === Qt.LeftButton) files.open(files.selected) }
                onWheel: event => window.changeZoom(event.angleDelta.y / 1200)
                Keys.onEscapePressed: { scrollMode = false; files.selected = -1 }
                Keys.onLeftPressed: { scrollMode = false; window.rotateView(-5, 0) }
                Keys.onRightPressed: { scrollMode = false; window.rotateView(5, 0) }
                Keys.onUpPressed: { scrollMode = false; window.rotateView(0, -5) }
                Keys.onDownPressed: { scrollMode = false; window.rotateView(0, 5) }
            }
            Label { palette: Theme.controlPalette(Theme.paletteRevision);
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 16
                text: files.busy ? files.cached ? "Refreshing cached view…" : "Loading…" : mouse.scrollMode ? "Scroll zoom active · move up/down · click or Escape to stop" : "LMB select / drag background or arrow keys to tilt · RMB actions · MMB pan / scroll-zoom"
                color: Theme.muted; font.pixelSize: 11
            }
            BusyIndicator { anchors.centerIn: parent; running: files.busy }
        }
        Inspector {
            Layout.preferredWidth: Math.min(310, window.width * .28); Layout.fillHeight: true
            onRequestAction: window.showActionsAt(window.width - 320, 80)
        }
    }
    Label { palette: Theme.controlPalette(Theme.paletteRevision);
        id: status
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: visible ? implicitHeight : 0
        visible: text.length > 0
        text: files.warning
        padding: 8; color: Theme.text; wrapMode: Text.WordWrap
    }
    FileActions { id: actions; objectName: "fileActions"; anchors.fill: parent }
    Shortcut { sequence: "Ctrl+C"; onActivated: files.clipboard(false) }
    Shortcut { sequence: "Ctrl+X"; onActivated: files.clipboard(true) }
    Shortcut { sequence: "Ctrl+V"; onActivated: files.paste() }
    Shortcut { sequence: "F2"; onActivated: actions.edit("rename") }
    Shortcut { sequence: "Delete"; onActivated: files.remove(false) }
    Shortcut { sequence: "Ctrl+Shift+N"; onActivated: actions.edit("mkdir") }
    Shortcut { sequence: "Alt+Up"; onActivated: files.up() }
    Shortcut { sequence: "F5"; onActivated: files.refresh() }
}
