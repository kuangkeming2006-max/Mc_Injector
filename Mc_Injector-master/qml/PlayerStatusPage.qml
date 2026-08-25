import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick3D
import QtQuick.Effects
import McOverlay 1.0

Item {
    id: root
    property color surfaceColor: "#FFFBFE"
    property color textColor: "#1D1B20"
    property color secondaryTextColor: "#49454F"
    property color primaryColor: "#6750A4"
    property real modelYaw: 25
    property real cameraDistance: 66

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 68
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: content
            x: 34; y: 28
            width: parent.width - 68
            spacing: 18

            Text { text: "Player Status"; color: root.textColor; font.pixelSize: 32; font.weight: Font.DemiBold }
            Text {
                text: "Live local-player identity and a draggable Minecraft skin preview"
                color: root.secondaryTextColor; font.pixelSize: 14
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 510
                spacing: 18

                Item {
                    id: skinCard
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 480

                    Item {
                        id: skinCardSource
                        anchors.fill: parent
                        visible: false
                        layer.enabled: true

                        Rectangle {
                            anchors.fill: parent
                            radius: 26
                            color: "#15121C"
                        }

                        View3D {
                        id: skinView
                        anchors.fill: parent
                        anchors.margins: 4
                        // Offscreen rendering makes the View3D obey the rounded
                        // parent clip. Inline rendering can extend through the
                        // four rounded corners on some graphics backends.
                        renderMode: View3D.Offscreen
                        environment: SceneEnvironment {
                            backgroundMode: SceneEnvironment.Color
                            clearColor: "#15121C"
                            antialiasingMode: SceneEnvironment.MSAA
                            antialiasingQuality: SceneEnvironment.VeryHigh
                        }

                        PerspectiveCamera {
                            id: camera
                            position: Qt.vector3d(0, 17, root.cameraDistance)
                            clipFar: 200
                            fieldOfView: 30
                        }
                        DirectionalLight { eulerRotation.x: -28; eulerRotation.y: -35; brightness: 1.25; castsShadow: true }
                        DirectionalLight { eulerRotation.x: 25; eulerRotation.y: 145; brightness: 0.55; color: "#D0BCFF" }

                        Texture {
                            id: skinTexture
                            source: SkinProfile.skinUrl
                            generateMipmaps: false
                            minFilter: Texture.Nearest
                            magFilter: Texture.Nearest
                            tilingModeHorizontal: Texture.ClampToEdge
                            tilingModeVertical: Texture.ClampToEdge
                            flipV: false
                        }
                        PrincipledMaterial {
                            id: skinMaterial
                            baseColorMap: skinTexture
                            roughness: 0.82
                            metalness: 0
                            cullMode: Material.NoCulling
                        }

                        Node {
                            id: playerModel
                            visible: SkinProfile.skinUrl.length > 0
                            eulerRotation: Qt.vector3d(-7, root.modelYaw, 0)
                            Model {
                                position: Qt.vector3d(0, 28, 0)
                                geometry: SkinCuboidGeometry { part: SkinCuboidGeometry.Head }
                                materials: [skinMaterial]
                            }
                            Model {
                                position: Qt.vector3d(0, 18, 0)
                                geometry: SkinCuboidGeometry { part: SkinCuboidGeometry.Body }
                                materials: [skinMaterial]
                            }
                            Model {
                                position: Qt.vector3d(-6, 18, 0)
                                geometry: SkinCuboidGeometry { part: SkinCuboidGeometry.RightArm }
                                materials: [skinMaterial]
                            }
                            Model {
                                position: Qt.vector3d(6, 18, 0)
                                geometry: SkinCuboidGeometry { part: SkinCuboidGeometry.LeftArm }
                                materials: [skinMaterial]
                            }
                            Model {
                                position: Qt.vector3d(-2, 6, 0)
                                geometry: SkinCuboidGeometry { part: SkinCuboidGeometry.RightLeg }
                                materials: [skinMaterial]
                            }
                            Model {
                                position: Qt.vector3d(2, 6, 0)
                                geometry: SkinCuboidGeometry { part: SkinCuboidGeometry.LeftLeg }
                                materials: [skinMaterial]
                            }
                        }

                        // Increment the current angle instead of restarting a
                        // fixed 20 -> 380 animation. That preserves the exact
                        // drag result when the mouse button is released.
                        Timer {
                            interval: 16
                            repeat: true
                            running: !skinMouse.dragging && SkinProfile.skinUrl.length > 0
                            onTriggered: root.modelYaw = (root.modelYaw + 0.32) % 360
                        }
                        }
                    }

                    Rectangle {
                        id: skinCardMask
                        anchors.fill: parent
                        radius: 26
                        color: "white"
                        antialiasing: true
                        visible: false
                        layer.enabled: true
                    }

                    // QQuickItem.clip is rectangular even when its Rectangle
                    // has a radius. Render the 3D card through a true rounded
                    // alpha mask so the four View3D corners cannot protrude.
                    MultiEffect {
                        anchors.fill: parent
                        source: skinCardSource
                        maskEnabled: true
                        maskSource: skinCardMask
                        autoPaddingEnabled: false
                    }

                    MouseArea {
                        id: skinMouse
                        anchors.fill: parent
                        property real previousX: 0
                        property bool dragging: pressed
                        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        onPressed: mouse => previousX = mouse.x
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            root.modelYaw += (mouse.x - previousX) * 0.65
                            previousX = mouse.x
                        }
                        onWheel: wheel => root.cameraDistance = Math.max(48, Math.min(92,
                                                                                     root.cameraDistance - wheel.angleDelta.y / 30))
                    }

                    BusyIndicator { anchors.centerIn: parent; running: SkinProfile.loading; visible: running }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom; anchors.bottomMargin: 18
                        text: SkinProfile.skinUrl.length > 0 ? "Drag to rotate · wheel to zoom" : (SkinProfile.errorMessage || "Waiting for player identity")
                        color: "#D0C8D7"; font.pixelSize: 12
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 360
                    Layout.fillHeight: true
                    radius: 26
                    color: root.surfaceColor
                    border.width: 1
                    border.color: "#E7E0EC"

                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 24; spacing: 18
                        Rectangle {
                            Layout.preferredWidth: 58; Layout.preferredHeight: 58; radius: 19
                            color: "#EADDFF"
                            Text { anchors.centerIn: parent; text: "P"; color: "#21005D"; font.pixelSize: 24; font.weight: Font.Bold }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: SkinProfile.displayName || OverlayManager.playerName || "Waiting for Minecraft"
                            color: root.textColor; font.pixelSize: 25; font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            text: OverlayManager.matchActive ? "BED WARS MATCH ACTIVE" : "NOT IN A MATCH"
                            color: OverlayManager.matchActive ? "#20853B" : root.secondaryTextColor
                            font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 0.8
                        }
                        Text { text: "Health"; color: root.secondaryTextColor; font.pixelSize: 12; font.weight: Font.Medium }
                        ProgressBar {
                            Layout.fillWidth: true
                            value: OverlayManager.playerMaxHealth > 0 ? OverlayManager.playerHealth / OverlayManager.playerMaxHealth : 0
                        }
                        Text {
                            text: OverlayManager.gameStateAvailable
                                  ? Number(OverlayManager.playerHealth).toFixed(1) + " / " + Number(OverlayManager.playerMaxHealth).toFixed(1) + " HP"
                                  : "—"
                            color: root.textColor; font.pixelSize: 20; font.weight: Font.DemiBold
                        }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#E7E0EC" }
                        Text { text: "Entity ID"; color: root.secondaryTextColor; font.pixelSize: 12 }
                        Text { text: OverlayManager.gameStateAvailable ? OverlayManager.playerEntityId : "—"; color: root.textColor; font.pixelSize: 18 }
                        Text { text: "Position"; color: root.secondaryTextColor; font.pixelSize: 12 }
                        Text {
                            Layout.fillWidth: true
                            text: OverlayManager.gameStateAvailable
                                  ? Number(OverlayManager.playerX).toFixed(2) + ", " + Number(OverlayManager.playerY).toFixed(2) + ", " + Number(OverlayManager.playerZ).toFixed(2)
                                  : "—"
                            color: root.textColor; font.pixelSize: 16; wrapMode: Text.WrapAnywhere
                        }
                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
    }
}
