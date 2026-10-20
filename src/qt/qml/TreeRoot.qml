// SPDX-License-Identifier: LGPL-2.1-or-later
import QtQuick
import QtQuick3D

Node {
    id: platform
    visible: files.layout === "tree" && files.root.length > 0
    position: files.treeRoot.center ?? Qt.vector3d(0, 0, 0)
    readonly property vector3d extent: files.treeRoot.extent ?? Qt.vector3d(1, 1, 1)
    Model {
        source: "#Cube"
        scale: Qt.vector3d(platform.extent.x / 100, platform.extent.y / 100, platform.extent.z / 100)
        materials: PrincipledMaterial { baseColor: "#dba43a"; roughness: .7 }
    }
    Model {
        source: "#Rectangle"
        y: platform.extent.y / 2 + Math.min(.3, platform.extent.y / 6)
        eulerRotation.x: -90
        scale: Qt.vector3d(platform.extent.x * .95 / 100, platform.extent.z * .95 / 100, 1)
        materials: PrincipledMaterial {
            lighting: PrincipledMaterial.NoLighting
            baseColorMap: Texture {
                sourceItem: Rectangle {
                    width: Math.max(1, 512 * platform.extent.x / Math.max(platform.extent.x, platform.extent.z))
                    height: Math.max(1, 512 * platform.extent.z / Math.max(platform.extent.x, platform.extent.z))
                    color: "#dba43a"
                    Text {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 6 }
                        height: parent.height * .28 - 12
                        text: files.treeRoot.name ?? ""
                        textFormat: Text.PlainText
                        color: "#231a08"
                        font.pixelSize: Math.max(12, Math.min(48, height - 4))
                        maximumLineCount: 2
                        wrapMode: Text.WrapAnywhere
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                minFilter: Texture.Linear
            }
            cullMode: Material.NoCulling
        }
    }
}
