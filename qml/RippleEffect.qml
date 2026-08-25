import QtQuick
import QtQuick.Effects

Item {
    id: root

    property color rippleColor: "#FFFFFF"
    property real peakOpacity: 0.18
    // QQuickItem.clip is always rectangular. Callers pass the visual
    // container's radius so MultiEffect can mask the animated ink to the same
    // rounded silhouette instead of exposing square corners.
    property real cornerRadius: 0

    visible: ripple.opacity > 0.001

    function burst(atX, atY) {
        ripple.x = atX - ripple.width / 2
        ripple.y = atY - ripple.height / 2
        rippleAnimation.restart()
    }

    Item {
        id: rippleSource
        anchors.fill: parent
        visible: false
        layer.enabled: true

        Rectangle {
            id: ripple
            width: 28
            height: width
            radius: width / 2
            color: root.rippleColor
            opacity: 0
            scale: 0.08
            transformOrigin: Item.Center
        }
    }

    Rectangle {
        id: roundedMask
        width: root.width
        height: root.height
        radius: Math.min(root.cornerRadius, Math.min(width, height) / 2)
        color: "white"
        antialiasing: true
        visible: false
        layer.enabled: true
    }

    MultiEffect {
        anchors.fill: parent
        source: rippleSource
        maskEnabled: true
        maskSource: roundedMask
        autoPaddingEnabled: false
    }

    ParallelAnimation {
        id: rippleAnimation

        // Material emphasized-decelerate: cubic-bezier(0.05, 0.7, 0.1, 1).
        // It expands quickly, then spends most of its time settling softly.
        NumberAnimation {
            target: ripple
            property: "scale"
            from: 0.08
            to: Math.max(2, Math.sqrt(root.width * root.width
                                     + root.height * root.height) * 2.15 / ripple.width)
            duration: 520
            easing.type: Easing.BezierSpline
            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
        }

        SequentialAnimation {
            NumberAnimation {
                target: ripple
                property: "opacity"
                from: 0
                to: root.peakOpacity
                duration: 70
                easing.type: Easing.OutQuad
            }
            NumberAnimation {
                target: ripple
                property: "opacity"
                to: 0
                duration: 450
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
            }
        }
    }
}
