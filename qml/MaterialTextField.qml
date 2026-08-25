import QtQuick
import QtQuick.Controls

TextField {
    id: control

    property color containerColor: "#F1ECF3"
    property color focusedColor: "#6750A4"
    property color textColor: "#1D1B20"
    property color placeholderColor: "#79747E"

    implicitHeight: 52
    leftPadding: 16
    rightPadding: 16
    topPadding: 8
    bottomPadding: 8
    color: textColor
    placeholderTextColor: placeholderColor
    selectionColor: "#D0BCFF"
    selectedTextColor: "#21005D"
    font.pixelSize: 14

    background: Rectangle {
        radius: 14
        color: control.containerColor
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? control.focusedColor : "#CAC4D0"

        Behavior on border.color { ColorAnimation { duration: 160 } }
        Behavior on color { ColorAnimation { duration: 160 } }
    }
}
