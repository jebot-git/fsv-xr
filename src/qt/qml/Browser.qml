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
    Connections { target: files; function onPropertiesRequested() {
        requestedProperties.selectedPath = files.selection.path || ""
        requestedProperties.restart()
    } }
    Timer {
        id: requestedProperties
        property string selectedPath: ""
        interval: 16; repeat: true
        onTriggered: {
            if (!selectedPath || files.selection.path !== selectedPath) stop()
            else if (!window.spatialPopups) { actions.showProperties(); stop() }
            else if (window.previewSelection()) stop()
        }
    }
    Timer {
        id: clickPreview
        property string selectedPath: ""
        interval: Qt.styleHints.mouseDoubleClickInterval
        onTriggered: { if (files.selection.path === selectedPath && !mouse.pressed) window.previewSelection() }
    }
    signal previewRequested(point anchor)
    signal contextMenuRequested(point anchor)
    function showActionsAt(x, y) {
        if (spatialPopups) contextMenuRequested(Qt.point(x, y))
        else actions.show(x, y)
    }
    function previewSelection() {
        if (!spatialPopups || files.selected < 0 || view.width <= 0 || view.height <= 0) return false
        const top = world.mapPositionToScene(world.selectionTop(files.selection))
        const projected = view.mapFrom3DScene(top)
        if (projected.z <= 0 || !isFinite(projected.x) || !isFinite(projected.y)) return false
        previewRequested(view.mapToItem(window, projected.x, projected.y))
        return true
    }
    function changeZoom(delta) {
        const target = Math.max(.15, Math.min(8, zoom * Math.exp(delta)))
        if (target === zoom) return
        const selection = files.selection
        const position = files.selected < 0 ? null : world.positionForZoom(world.selectionTop(selection), target / zoom)
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
            Label { palette: Theme.controlPalette(Theme.paletteRevision); text: " fsvr "; font.bold: true; font.pixelSize: 22 }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "↑ Up"; enabled: !files.operating; onClicked: files.up() }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "↻"; enabled: !files.operating; onClicked: files.refresh() }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "Empty Trash…"; visible: files.inTrash; enabled: !files.busy; onClicked: actions.confirmEmptyTrash() }
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
                property bool fileDrag: false
                property bool dragged: false
                onPressed: event => {
                    clickPreview.stop()
                    scrollWasActive = scrollMode
                    scrollMode = false
                    previous = Qt.point(event.x, event.y)
                    start = previous
                    forceActiveFocus()
                    rotating = false
                    dragged = false
                    fileDrag = false
                    if (event.button !== Qt.MiddleButton) {
                        const hit = view.pick(event.x, event.y)
                        files.selected = hit.objectHit ? hit.objectHit.entryIndex : -1
                        // Latch at press time: crossing an object during a
                        // background drag must not interrupt rotation.
                        rotating = event.button === Qt.LeftButton
                            && (!hit.objectHit || !!(event.modifiers & Qt.AltModifier))
                    }
                    fileDrag = event.button === Qt.LeftButton && !rotating && files.selected >= 0 && !files.busy && !files.inTrash
                    if (event.button === Qt.RightButton) {
                        const point = mouse.mapToItem(window, event.x, event.y)
                        window.showActionsAt(point.x, point.y)
                    }
                }
                onPositionChanged: event => {
                    if (fileDrag && (pressedButtons & Qt.LeftButton)
                            && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) >= Qt.styleHints.startDragDistance) {
                        fileDrag = false
                        dragged = true
                        clickPreview.stop()
                        files.startDrag()
                        return
                    }
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
                    if (!dragged && event.button === Qt.LeftButton && !(event.modifiers & Qt.AltModifier) && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) < 5 && window.spatialPopups) {
                        // Allow a real mouse double-click to open a directory
                        // before a preview popup can take the pointer grab.
                        clickPreview.selectedPath = files.selection.path || ""
                        clickPreview.restart()
                    }
                    // A short MMB click enables scroll-button zoom, usable with DomeD's
                    // middle/thumb pinch without needing a keyboard modifier.
                    if (!scrollWasActive && event.button === Qt.MiddleButton && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) < 5) {
                        scrollMode = true
                    }
                }
                hoverEnabled: true
                onCanceled: { fileDrag = false; clickPreview.stop(); scrollMode = false; rotating = false }
                onExited: { if (!pressed) scrollMode = false }
                Connections { target: window; function onActiveChanged() { if (!window.active) { mouse.scrollMode = false } } }
                onDoubleClicked: event => { clickPreview.stop(); if (event.button === Qt.LeftButton) files.open(files.selected) }
                onWheel: event => { clickPreview.stop(); window.changeZoom(event.angleDelta.y ? event.angleDelta.y / 1200 : event.pixelDelta.y / 600); event.accepted = true }
                Keys.onEscapePressed: { scrollMode = false; files.selected = -1 }
                Keys.onLeftPressed: { scrollMode = false; window.rotateView(-5, 0) }
                Keys.onRightPressed: { scrollMode = false; window.rotateView(5, 0) }
                Keys.onUpPressed: { scrollMode = false; window.rotateView(0, -5) }
                Keys.onDownPressed: { scrollMode = false; window.rotateView(0, 5) }
            }
            DropArea {
                id: fileDrop
                objectName: "fileDropArea"
                anchors.fill: parent
                property string directory: ""
                function updateTarget(drag) {
                    const hit = view.pick(drag.x, drag.y)
                    const index = hit.objectHit ? hit.objectHit.entryIndex : -1
                    directory = files.dropDirectory(index)
                    drag.accepted = drag.hasUrls && files.canDrop(drag.urls, directory)
                }
                onEntered: drag => updateTarget(drag)
                onPositionChanged: drag => updateTarget(drag)
                onDropped: drop => {
                    updateTarget(drop)
                    if (drop.accepted && files.dropFiles(drop.urls, directory, drop.proposedAction))
                        drop.acceptProposedAction()
                    else drop.accepted = false
                }
                Rectangle {
                    anchors.fill: parent
                    visible: fileDrop.containsDrag
                    color: "transparent"
                    border.width: 2; border.color: Theme.colors.highlight
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top; anchors.topMargin: 12
                        text: "Drop into " + fileDrop.directory
                        color: Theme.text
                    }
                }
            }
            Label { palette: Theme.controlPalette(Theme.paletteRevision);
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 16
                text: files.busy ? files.cached ? "Refreshing cached view…" : "Loading…" : mouse.scrollMode ? "Scroll zoom active · move up/down · click or Escape to stop" : "LMB select / drag files · drag background or arrow keys to tilt · RMB actions · MMB pan / scroll-zoom"
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
    Shortcut { sequence: "Delete"; onActivated: { if (files.inTrash) actions.confirmRemoval(); else files.remove(false) } }
    Shortcut { sequence: "Ctrl+Shift+N"; onActivated: actions.edit("mkdir") }
    Shortcut { sequence: "Alt+Up"; onActivated: files.up() }
    Shortcut { sequence: "F5"; onActivated: files.refresh() }
}
