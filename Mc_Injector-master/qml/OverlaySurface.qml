import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import McOverlay 1.0

Window {
    id: overlayWindow

    visible: false
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.Tool
    title: "Java Overlay Surface"

    // This QQuickWindow is rendered by the same D3D11 scene graph as main.qml.
    // OverlayManager owns its native hit-testing and position; QML owns only the
    // pixels and interactive content.
    Item {
        anchors.fill: parent

        Rectangle {
            anchors.centerIn: parent
            width: 2
            height: 28
            color: "#80D0BCFF"
            visible: OverlayManager.attached
        }
        Rectangle {
            anchors.centerIn: parent
            width: 28
            height: 2
            color: "#80D0BCFF"
            visible: OverlayManager.attached
        }

        Rectangle {
            id: hudShadow
            anchors.right: hud.right
            anchors.top: hud.top
            anchors.rightMargin: -5
            anchors.topMargin: 7
            width: hud.width
            height: hud.height
            radius: hud.radius
            color: "#59000000"
        }

        Rectangle {
            id: hud
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 24
            width: Math.min(292, parent.width - 48)
            height: 146
            radius: 22
            color: "#E9211E24"
            border.width: 1
            border.color: "#55FFFFFF"
            clip: true
            scale: OverlayManager.attached ? 1 : 0.84
            opacity: OverlayManager.attached ? 1 : 0
            transformOrigin: Item.TopRight

            Behavior on scale {
                NumberAnimation {
                    duration: 420
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                }
            }
            Behavior on opacity { NumberAnimation { duration: 260 } }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: "JAVA OVERLAY"
                        color: "#D0BCFF"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        font.letterSpacing: 1.1
                    }
                    Rectangle {
                        Layout.preferredWidth: modeText.implicitWidth + 14
                        Layout.preferredHeight: 23
                        radius: 12
                        color: OverlayManager.interactive ? "#4F378B" : "#343038"
                        Text {
                            id: modeText
                            anchors.centerIn: parent
                            text: OverlayManager.interactive ? "INTERACTIVE" : "PASSTHROUGH"
                            color: "#F4EEFA"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: OverlayManager.targetTitle
                    color: "white"
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    text: "PID " + OverlayManager.targetPid
                    color: "#CAC4D0"
                    font.pixelSize: 11
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    visible: OverlayManager.interactive
                    Text {
                        Layout.fillWidth: true
                        text: "Pointer input is active"
                        color: "#E7E0EC"
                        font.pixelSize: 11
                    }
                    MaterialButton {
                        Layout.preferredWidth: 86
                        Layout.preferredHeight: 32
                        compact: true
                        text: "Done"
                        filled: false
                        foregroundColor: "#D0BCFF"
                        outlineColor: "#7C7284"
                        onClicked: OverlayManager.interactive = false
                    }
                }
            }
        }
    }
}

