// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick3D

ThemedPanel {
    id: window
    objectName: "fileBrowser"
    width: 1280; height: 820
    color: spatialPopups ? "transparent" : Theme.sceneBackground
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    property real panZ: 0
    property real yaw: 0
    property real pitch: 0
    property bool nativeMenus: false
    // Menu callbacks and keyboard shortcuts share the existing dialogs. Text
    // editing keeps clipboard ownership instead of changing selected files.
    function menuCommand(command) {
        const focus = Window.window ? Window.window.activeFocusItem : null
        if (["cut", "copy", "paste"].includes(command)) {
            if (focus && typeof focus[command] === "function") focus[command]()
            else if (!files.busy && !files.inTrash && !files.inArchive) {
                if (command === "paste") files.paste()
                else if (files.selected >= 0) files.clipboard(command === "cut")
            }
        } else if (command === "location") { location.forceActiveFocus(); location.selectAll() }
        else if (command === "properties") showInspector()
        else if (command === "delete") actions.confirmRemoval()
        else if (command === "emptyTrash") actions.confirmEmptyTrash()
        else if (command === "resetView") resetView()
        else actions.edit(command)
    }
    function resetView() { zoom = 1; panX = 0; panY = 0; panZ = 0; yaw = 0; pitch = 0 }
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
    Connections { target: files; function onPropertiesRequested() { window.showInspector() } }
    function showInspector() { inspector.reveal() }
    signal contextMenuRequested(point anchor)
    function showActionsAt(x, y) {
        if (spatialPopups) contextMenuRequested(Qt.point(x, y))
        else actions.show(x, y)
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
    ToolBar {
        id: toolbar
        palette: Theme.controlPalette(Theme.paletteRevision)
        anchors { top: parent.top; left: parent.left; right: parent.right }
        padding: 6
        contentItem: GridLayout {
            readonly property bool compact: window.width < 900
            columns: compact ? 3 : 5
            columnSpacing: 6; rowSpacing: 4
            RowLayout {
                spacing: 4
                Label { palette: Theme.controlPalette(Theme.paletteRevision); text: "fsvr"; font.bold: true; font.pixelSize: 22 }
                ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "↑ Up"; enabled: !files.operating; onClicked: files.up() }
                ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "↻"; enabled: !files.operating; onClicked: files.refresh() }
            }
            TextField {
                id: location
                objectName: "locationField"
                palette: Theme.controlPalette(Theme.paletteRevision)
                Layout.fillWidth: true; Layout.minimumWidth: 40
                Layout.columnSpan: parent.compact ? 2 : 1
                text: files.root; onAccepted: files.navigate(text)
            }
            ComboBox {
                palette: Theme.controlPalette(Theme.paletteRevision)
                Layout.preferredWidth: 120
                model: ["Map", "Tree", "Disc"]
                currentIndex: ["map", "tree", "disc"].indexOf(files.layout)
                onActivated: files.layout = ["map", "tree", "disc"][currentIndex]
            }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "Reset View"; onClicked: window.resetView() }
            ToolButton { palette: Theme.controlPalette(Theme.paletteRevision); text: "Empty Trash…"; visible: files.inTrash; enabled: !files.busy; onClicked: actions.confirmEmptyTrash() }
        }
    }
    Tabs {
        id: tabs
        shortcutsEnabled: !window.nativeMenus
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
                    frustumCullingEnabled: true
                }
                camera: camera
                World {
                    id: world
                    zoom: window.zoom; panX: window.panX; panY: window.panY; panZ: window.panZ
                    yaw: window.yaw; pitch: window.pitch; showBase: !window.spatialPopups
                    // Two texture pixels per projected pixel, rounded to tiers
                    // by World to avoid reallocating textures on every gesture.
                    texturePixelsPerUnit: 2 * view.height * window.zoom
                        / (2 * Math.tan(camera.fieldOfView * Math.PI / 360)
                           * Math.max(20, camera.scenePosition.minus(world.scenePosition).length()))
                }
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
                onPressed: event => {
                    scrollWasActive = scrollMode
                    scrollMode = false
                    previous = Qt.point(event.x, event.y)
                    start = previous
                    forceActiveFocus()
                    rotating = false
                    fileDrag = false
                    if (event.button !== Qt.MiddleButton) {
                        const hit = view.pick(event.x, event.y)
                        files.selected = hit.objectHit ? hit.objectHit.entryIndex : -1
                        // Latch at press time: crossing an object during a
                        // background drag must not interrupt rotation.
                        rotating = event.button === Qt.LeftButton
                            && (!hit.objectHit || !!(event.modifiers & Qt.AltModifier))
                    }
                    fileDrag = event.button === Qt.LeftButton && !rotating && files.selected >= 0 && !files.busy && !files.inTrash && !files.inArchive
                    if (event.button === Qt.RightButton) {
                        const point = mouse.mapToItem(window, event.x, event.y)
                        window.showActionsAt(point.x, point.y)
                    }
                }
                onPositionChanged: event => {
                    if (fileDrag && (pressedButtons & Qt.LeftButton)
                            && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) >= Qt.styleHints.startDragDistance) {
                        fileDrag = false
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
                    // A short MMB click enables scroll-button zoom without
                    // needing to hold a mouse button or keyboard modifier.
                    if (!scrollWasActive && event.button === Qt.MiddleButton && Math.abs(event.x - start.x) + Math.abs(event.y - start.y) < 5) {
                        scrollMode = true
                    }
                }
                hoverEnabled: true
                onCanceled: { fileDrag = false; scrollMode = false; rotating = false }
                onExited: { if (!pressed) scrollMode = false }
                Connections { target: window; function onActiveChanged() { if (!window.active) { mouse.scrollMode = false } } }
                onDoubleClicked: event => { if (event.button === Qt.LeftButton) files.open(files.selected) }
                onWheel: event => { window.changeZoom(event.angleDelta.y ? event.angleDelta.y / 1200 : event.pixelDelta.y / 600); event.accepted = true }
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
                width: Math.max(0, parent.width - 32)
                wrapMode: Text.WordWrap
                color: Theme.muted; font.pixelSize: 11
            }
            BusyIndicator { anchors.centerIn: parent; running: files.busy }
        }
        Inspector {
            id: inspector
            objectName: "fileInspector"
            Layout.preferredWidth: Math.min(310, window.width * .28); Layout.fillHeight: true
            onRequestAction: {
                const anchor = inspector.mapToItem(window, 12, inspector.height - 60)
                window.showActionsAt(anchor.x, anchor.y)
            }
        }
    }
    Label { palette: Theme.controlPalette(Theme.paletteRevision);
        id: status
        objectName: "statusBar"
        height: visible ? implicitHeight : 0
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        visible: text.length > 0
        text: files.warning
        padding: 8; color: Theme.text; wrapMode: Text.WordWrap
    }
    FileActions { id: actions; objectName: "fileActions"; anchors.fill: parent; onInspectorRequested: window.showInspector() }
    Shortcut { enabled: !window.nativeMenus; sequence: "Ctrl+C"; onActivated: files.clipboard(false) }
    Shortcut { enabled: !window.nativeMenus; sequence: "Ctrl+X"; onActivated: files.clipboard(true) }
    Shortcut { enabled: !window.nativeMenus; sequence: "Ctrl+V"; onActivated: files.paste() }
    Shortcut { enabled: !window.nativeMenus; sequence: "F2"; onActivated: actions.edit("rename") }
    Shortcut { enabled: !window.nativeMenus; sequence: "Delete"; onActivated: { if (files.inArchive) return; if (files.inTrash) actions.confirmRemoval(); else files.remove(false) } }
    Shortcut { enabled: !window.nativeMenus; sequence: "Ctrl+Shift+N"; onActivated: actions.edit("mkdir") }
    Shortcut { enabled: !window.nativeMenus; sequence: "Alt+Up"; onActivated: files.up() }
    Shortcut { enabled: !window.nativeMenus; sequence: "F5"; onActivated: files.refresh() }
}
