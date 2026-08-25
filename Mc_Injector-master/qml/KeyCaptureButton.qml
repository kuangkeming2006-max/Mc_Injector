import QtQuick
import QtQuick.Controls

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

    onCapturingChanged: {
        if (capturing) {
            // Mouse release can move active focus back to the MouseArea on
            // Windows. Defer focus acquisition until that release has been
            // dispatched so the very next physical key reaches keySink.
            Qt.callLater(function() {
                if (root.capturing)
                    keySink.forceActiveFocus(Qt.ShortcutFocusReason)
            })
        }
    }

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

    function fallbackVirtualKey(event) {
        if (event.key >= Qt.Key_A && event.key <= Qt.Key_Z)
            return event.key
        if (event.key >= Qt.Key_0 && event.key <= Qt.Key_9)
            return event.key
        if (event.key >= Qt.Key_F1 && event.key <= Qt.Key_F12)
            return 112 + event.key - Qt.Key_F1
        const keys = {}
        keys[Qt.Key_Backspace] = 8; keys[Qt.Key_Tab] = 9
        keys[Qt.Key_Return] = 13; keys[Qt.Key_Enter] = 13
        keys[Qt.Key_Space] = 32; keys[Qt.Key_PageUp] = 33
        keys[Qt.Key_PageDown] = 34; keys[Qt.Key_End] = 35
        keys[Qt.Key_Home] = 36; keys[Qt.Key_Left] = 37
        keys[Qt.Key_Up] = 38; keys[Qt.Key_Right] = 39
        keys[Qt.Key_Down] = 40; keys[Qt.Key_Insert] = 45
        keys[Qt.Key_Delete] = 46; keys[Qt.Key_Apostrophe] = 222
        return keys[event.key] || 0
    }

    function captureEvent(event) {
        if (!root.capturing)
            return
        event.accepted = true
        if (event.key === Qt.Key_Escape) {
            root.capturing = false
            return
        }
        let key = Number(event.nativeVirtualKey)
        if (key < 8 || key > 254)
            key = root.fallbackVirtualKey(event)
        if (key >= 8 && key <= 254 && key !== 27) {
            root.virtualKey = key
            root.capturing = false
            root.keyCaptured(key)
        }
    }

    // A real focusable key sink is more reliable than attaching Keys to the
    // FocusScope itself. In particular, QQuickWindow otherwise restores focus
    // to the MouseArea after a click and the Controller never sees key presses.
    TextInput {
        id: keySink
        width: 1
        height: 1
        opacity: 0
        enabled: root.capturing
        focus: root.capturing
        activeFocusOnPress: false
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function(event) { root.captureEvent(event) }
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
            root.capturing = true
        }
    }
}
