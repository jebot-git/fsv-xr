// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick3D

Node {
    id: world
    objectName: "filesystemWorld"
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    property real panZ: 0
    property real yaw: 0
    property real pitch: 0
    property bool showBase: true
    scale: Qt.vector3d(zoom, zoom, zoom)
    position: Qt.vector3d(panX, panY, panZ)
    eulerRotation.y: yaw
    eulerRotation.x: pitch
    function positionForZoom(anchor, ratio) {
        // Compensate the full rotated/scaled offset, including height, so the
        // selected top stays under the same screen position during scaling.
        const point = mapPositionToScene(anchor)
        return Qt.vector3d(x + (point.x - x) * (1 - ratio),
                           y + (point.y - y) * (1 - ratio),
                           z + (point.z - z) * (1 - ratio))
    }

    DirectionalLight { eulerRotation: Qt.vector3d(-55, -25, 0); brightness: 1.2; ambientColor: "#8195a5" }
    Model {
        visible: world.showBase
        source: "#Cube"
        y: -2
        scale: Qt.vector3d(1.72, .02, 1.32)
        materials: PrincipledMaterial { baseColor: Theme.surface; roughness: .9 }
    }
    Repeater3D {
        model: files
        delegate: Node {
            id: entry
            required property int index
            required property vector3d center
            required property vector3d extent
            required property color tint
            required property string thumbnail
            required property string fileName
            required property bool isDirectory
            position: center
            Model {
                property int entryIndex: entry.index
                source: "#Cube"
                pickable: true
                scale: Qt.vector3d(entry.extent.x / 100, entry.extent.y / 100, entry.extent.z / 100)
                materials: PrincipledMaterial {
                    baseColor: entry.isDirectory ? (files.selected === entry.index ? Qt.lighter(entry.tint, 1.2) : entry.tint)
                        : files.selected === entry.index ? Theme.accent : entry.tint
                    roughness: .7
                }
            }
            Model {
                id: top
                readonly property bool rotated: !entry.isDirectory && entry.extent.z > entry.extent.x * 1.4
                property int entryIndex: entry.index
                source: "#Rectangle"
                pickable: true
                y: entry.extent.y / 2 + .3
                eulerRotation.x: -90
                eulerRotation.z: rotated ? 90 : 0
                scale: Qt.vector3d((rotated ? entry.extent.z : entry.extent.x) * .95 / 100,
                                  (rotated ? entry.extent.x : entry.extent.z) * .95 / 100, 1)
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColorMap: Texture {
                        sourceItem: Item {
                            // Match the physical top's aspect ratio. A square
                            // source stretched over a treemap tile distorts
                            // both the image and the text, even with AspectFit.
                            readonly property real pixelsPerUnit: 512 / Math.max(entry.extent.x, entry.extent.z)
                            width: Math.max(1, Math.round((top.rotated ? entry.extent.z : entry.extent.x) * pixelsPerUnit))
                            height: Math.max(1, Math.round((top.rotated ? entry.extent.x : entry.extent.z) * pixelsPerUnit))
                            Rectangle {
                                anchors.fill: parent
                                color: entry.isDirectory ? entry.tint : Theme.colors.base
                            }
                            Image {
                                objectName: "objectThumbnail" + entry.index
                                anchors { top: parent.top; left: parent.left; right: parent.right; bottom: filenameBand.top }
                                sourceSize: Qt.size(256, 256)
                                source: entry.isDirectory ? "" : entry.thumbnail
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                            }
                            // Keep the name separate from the preview so image,
                            // text and external thumbnailers cannot obscure it.
                            Rectangle {
                                id: filenameBand
                                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                                height: entry.isDirectory ? parent.height * .28
                                    : Math.min(parent.height * .5, Math.max(64, parent.height * .28))
                                color: entry.isDirectory ? entry.tint : Theme.colors.base
                                Text {
                                    objectName: "objectFilename" + entry.index
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    text: entry.fileName
                                    textFormat: Text.PlainText
                                    color: entry.isDirectory ? "#231a08" : Theme.colors.text
                                    font.pixelSize: Math.max(12, Math.min(48, height - 4))
                                    wrapMode: Text.WrapAnywhere
                                    maximumLineCount: 2
                                    elide: Text.ElideMiddle
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                        generateMipmaps: true
                        minFilter: Texture.Linear
                        mipFilter: Texture.Linear
                    }
                    cullMode: Material.NoCulling
                }
            }
        }
    }
}
