import QtQuick
import QtQuick.Controls
import McOverlay 1.0

FocusScope {
    id: root

    property int virtualKey: 222
    property bool capturing: false
    property color primaryColor: "#6750A4"
    property color surfaceColor: "#FFFBFE"
    property color textColor: "#1D1B20"
    signal keyCaptured(int virtualKey)

    implicitWidth: 190
    implicitHeight: 48
    activeFocusOnTab: true

    onVisibleChanged: if (!visible && capturing) HotkeyCapture.cancelCapture()
    Component.onDestruction: if (capturing) HotkeyCapture.cancelCapture()

    function keyLabel(key) {
        const names = {
            8: "Backspace", 9: "Tab", 13: "Enter", 16: "Shift",
            17: "Ctrl", 18: "Alt", 19: "Pause", 20: "Caps Lock",
            27: "Escape", 32: "Space", 33: "Page Up", 34: "Page Down",
            35: "End", 36: "Home", 37: "Left", 38: "Up", 39: "Right",
            40: "Down", 45: "Insert", 46: "Delete", 91: "Left Windows",
            92: "Right Windows", 112: "F1", 113: "F2", 114: "F3",
            115: "F4", 116: "F5", 117: "F6", 118: "F7", 119: "F8",
            120: "F9", 121: "F10", 122: "F11", 123: "F12",
            160: "Left Shift", 161: "Right Shift", 162: "Left Ctrl",
            163: "Right Ctrl", 164: "Left Alt", 165: "Right Alt",
            186: ";", 187: "=", 188: ",", 189: "-", 190: ".",
            191: "/", 192: "`", 219: "[", 220: "\\", 221: "]",
            222: "Apostrophe (')"
        }
        if (names[key] !== undefined)
            return names[key]
        if ((key >= 48 && key <= 57) || (key >= 65 && key <= 90))
            return String.fromCharCode(key)
        return "Key " + key
    }

    Connections {
        target: HotkeyCapture
        function onCaptureStarted() { root.capturing = false }
        function onCaptureCanceled() { root.capturing = false }
        function onKeyCaptured(key) {
            if (!root.capturing)
                return
            root.virtualKey = key
            root.capturing = false
            root.keyCaptured(key)
        }
    }

    Rectangle {
        id: container
        anchors.fill: parent
        radius: 16
        color: root.capturing ? "#EADDFF" : root.surfaceColor
        border.width: root.activeFocus || root.capturing ? 2 : 1
        border.color: root.activeFocus || root.capturing ? root.primaryColor : "#CAC4D0"
        scale: captureMouse.pressed ? 0.985 : 1.0

        Behavior on color { ColorAnimation { duration: 180 } }
        Behavior on border.color { ColorAnimation { duration: 180 } }
        Behavior on scale {
            NumberAnimation {
                duration: 220
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
            }
        }

        RippleEffect {
            id: ripple
            anchors.fill: parent
            cornerRadius: container.radius
            rippleColor: root.primaryColor
            peakOpacity: 0.13
        }

        Text {
            anchors.centerIn: parent
            text: root.capturing ? "Press a key…" : root.keyLabel(root.virtualKey)
            color: root.capturing ? "#21005D" : root.textColor
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
    }

    MouseArea {
        id: captureMouse
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onPressed: function(mouse) {
            ripple.burst(mouse.x, mouse.y)
        }
        onClicked: function(mouse) {
            HotkeyCapture.beginCapture()
            root.capturing = true
        }
    }
}
