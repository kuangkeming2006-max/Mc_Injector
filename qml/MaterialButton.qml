import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string text: "Button"
    property string iconText: ""
    property bool filled: true
    property bool compact: false
    property color containerColor: "#6750A4"
    property color foregroundColor: filled ? "#FFFFFF" : "#6750A4"
    property color outlineColor: "#79747E"
    property string toolTip: ""
    signal clicked()

    implicitWidth: Math.max(compact ? 48 : 104, labelRow.implicitWidth + (compact ? 24 : 40))
    implicitHeight: compact ? 42 : 48
    enabled: true
    opacity: enabled ? 1 : 0.38
    scale: buttonMouse.pressed ? 0.965 : (buttonMouse.containsMouse ? 1.025 : 1.0)
    transformOrigin: Item.Center
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: if (root.enabled) root.clicked()

    Keys.onSpacePressed: if (enabled) clicked()
    Keys.onReturnPressed: if (enabled) clicked()

    Behavior on scale {
        NumberAnimation {
            duration: 260
            easing.type: Easing.BezierSpline
            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
        }
    }

    Rectangle {
        id: shadow
        anchors.fill: container
        anchors.topMargin: buttonMouse.containsMouse ? 5 : 3
        anchors.leftMargin: 2
        anchors.rightMargin: -2
        anchors.bottomMargin: -3
        radius: container.radius
        color: "#18000000"
        opacity: root.filled ? (buttonMouse.containsMouse ? 0.9 : 0.55) : 0

        Behavior on anchors.topMargin { NumberAnimation { duration: 180 } }
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    Rectangle {
        id: container
        anchors.fill: parent
        radius: height / 2
        color: root.filled
               ? (buttonMouse.containsMouse ? Qt.lighter(root.containerColor, 1.08)
                                            : root.containerColor)
               : (buttonMouse.containsMouse ? "#126750A4" : "transparent")
        border.width: root.filled ? 0 : 1
        border.color: root.outlineColor

        Behavior on color { ColorAnimation { duration: 160 } }

        RippleEffect {
            id: ripple
            anchors.fill: parent
            rippleColor: root.filled ? "#FFFFFF" : root.foregroundColor
            cornerRadius: container.radius
        }

        RowLayout {
            id: labelRow
            anchors.centerIn: parent
            spacing: 8

            Text {
                visible: root.iconText.length > 0
                text: root.iconText
                color: root.foregroundColor
                font.pixelSize: root.compact ? 17 : 19
                font.weight: Font.DemiBold
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                visible: !root.compact || root.text.length > 0
                text: root.text
                color: root.foregroundColor
                font.pixelSize: 14
                font.weight: Font.DemiBold
                Layout.alignment: Qt.AlignVCenter
            }
        }

        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            enabled: root.enabled
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onPressed: function(mouse) {
                root.forceActiveFocus()
                ripple.burst(mouse.x, mouse.y)
            }
            onClicked: root.clicked()
        }
    }

    ToolTip.visible: toolTip.length > 0 && buttonMouse.containsMouse
    ToolTip.text: toolTip
    ToolTip.delay: 650
}
