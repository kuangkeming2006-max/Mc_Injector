import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property int processPid: 0
    property string executableName: "javaw.exe"
    property string executablePath: ""
    property string windowTitle: "Java process"
    property string memoryText: "Unavailable"
    property bool hasWindow: false
    property bool selected: false
    property bool sessionProcess: false
    property bool titleExpanded: false
    property int entryDelay: 0
    property color primaryColor: "#6750A4"
    property color surfaceColor: "#FFFBFE"
    property color textColor: "#1D1B20"
    property color secondaryTextColor: "#49454F"

    // Public sizing/state hooks allow the owning view to reserve enough row
    // space while this card reveals a wrapped window title.
    readonly property int collapsedHeight: 166
    readonly property bool titleExpandable: titleOverflowDetected
    readonly property int expandedHeight: collapsedHeight + Math.ceil(
                                              Math.max(0,
                                                       titleExpandedMeasure.paintedHeight
                                                       - titleLineMetric.paintedHeight))
    property bool titleOverflowDetected: false

    signal selectRequested(int pid)
    signal attachRequested(int pid)

    implicitHeight: titleExpanded ? expandedHeight : collapsedHeight
    height: implicitHeight
    opacity: 0
    scale: cardMouse.pressed ? 0.985 : (cardMouse.containsMouse ? 1.018 : 1.0)
    transformOrigin: Item.Center
    activeFocusOnTab: true

    Accessible.role: Accessible.ListItem
    Accessible.name: windowTitle + ", PID " + processPid

    transform: Translate { id: entryTranslation; y: 22 }

    function refreshTitleOverflow() {
        if (!titleExpanded)
            titleOverflowDetected = titleLabel.truncated
    }

    function toggleTitleExpanded() {
        if (titleExpanded || titleExpandable)
            titleExpanded = !titleExpanded
    }

    onWindowTitleChanged: {
        titleExpanded = false
        titleOverflowDetected = false
        Qt.callLater(refreshTitleOverflow)
    }

    onTitleExpandedChanged: {
        if (!titleExpanded)
            Qt.callLater(refreshTitleOverflow)
    }

    // Material emphasized-decelerate: cubic-bezier(0.05, 0.7, 0.1, 1).
    // Height settles softly so the wrapped title does not make the card jump.
    Behavior on height {
        NumberAnimation {
            duration: 360
            easing.type: Easing.BezierSpline
            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
        }
    }

    Behavior on scale {
        NumberAnimation {
            duration: 300
            easing.type: Easing.BezierSpline
            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
        }
    }

    Rectangle {
        id: shadow
        x: 4
        y: cardMouse.containsMouse ? 11 : 7
        width: root.width - 8
        height: root.height - 2
        radius: 22
        color: root.selected ? "#2A6750A4" : "#19000000"
        opacity: cardMouse.containsMouse ? 0.9 : 0.55

        Behavior on y {
            NumberAnimation {
                duration: 260
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
            }
        }
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 2
        radius: 20
        color: root.selected ? "#F2ECFF" : root.surfaceColor
        border.width: root.selected ? 2 : 1
        border.color: root.selected ? root.primaryColor : "#E7E0EC"
        clip: true

        Behavior on color { ColorAnimation { duration: 220 } }
        Behavior on border.color { ColorAnimation { duration: 220 } }

        RippleEffect {
            id: ripple
            anchors.fill: parent
            rippleColor: root.primaryColor
            peakOpacity: 0.12
            cornerRadius: card.radius
        }

        RowLayout {
            z: 1
            anchors.fill: parent
            anchors.margins: 18
            spacing: 16

            Rectangle {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 48
                Layout.alignment: Qt.AlignTop
                radius: 16
                color: root.selected ? root.primaryColor : "#EADDFF"

                Text {
                    anchors.centerIn: parent
                    text: "J"
                    color: root.selected ? "#FFFFFF" : "#21005D"
                    font.pixelSize: 22
                    font.weight: Font.Bold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 5

                RowLayout {
                    id: titleRow
                    Layout.fillWidth: true
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            id: titleLabel
                            Layout.fillWidth: true
                            text: root.windowTitle
                            color: root.textColor
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            wrapMode: root.titleExpanded ? Text.Wrap : Text.NoWrap
                            elide: root.titleExpanded ? Text.ElideNone : Text.ElideRight
                            maximumLineCount: root.titleExpanded ? 2147483647 : 1

                            onTruncatedChanged: root.refreshTitleOverflow()
                            onWidthChanged: Qt.callLater(root.refreshTitleOverflow)
                            Component.onCompleted: Qt.callLater(root.refreshTitleOverflow)
                        }

                        Rectangle {
                            id: titleToggle
                            Layout.preferredWidth: titleToggleLabel.implicitWidth + 18
                            Layout.preferredHeight: 22
                            visible: root.titleExpanded || root.titleExpandable
                            radius: 11
                            color: titleToggleMouse.containsMouse ? "#E8DEF8" : "transparent"
                            border.width: 1
                            border.color: root.titleExpanded ? root.primaryColor : "#CAC4D0"
                            activeFocusOnTab: visible

                            Accessible.role: Accessible.Button
                            Accessible.name: root.titleExpanded
                                             ? "Collapse full process window title"
                                             : "Expand full process window title"

                            Behavior on color { ColorAnimation { duration: 160 } }
                            Behavior on border.color { ColorAnimation { duration: 160 } }

                            Text {
                                id: titleToggleLabel
                                anchors.centerIn: parent
                                text: root.titleExpanded ? "Less  −" : "More  +"
                                color: root.primaryColor
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.3
                            }

                            RippleEffect {
                                id: titleToggleRipple
                                anchors.fill: parent
                                rippleColor: root.primaryColor
                                peakOpacity: 0.13
                                cornerRadius: titleToggle.radius
                            }

                            MouseArea {
                                id: titleToggleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onPressed: function(mouse) {
                                    titleToggle.forceActiveFocus()
                                    titleToggleRipple.burst(mouse.x, mouse.y)
                                }
                                onClicked: root.toggleTitleExpanded()
                            }

                            Keys.onSpacePressed: root.toggleTitleExpanded()
                            Keys.onReturnPressed: root.toggleTitleExpanded()

                            ToolTip.visible: titleToggleMouse.containsMouse
                            ToolTip.text: root.titleExpanded
                                          ? "Collapse window title"
                                          : "Show full window title"
                        }
                    }

                    Rectangle {
                        id: statusBadge
                        Layout.preferredWidth: statusLabel.implicitWidth + 16
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignTop
                        radius: 13
                        color: root.hasWindow ? "#D7F7DD" : "#F2E7EC"

                        Text {
                            id: statusLabel
                            anchors.centerIn: parent
                            text: root.hasWindow ? "WINDOW READY" : "HEADLESS"
                            color: root.hasWindow ? "#155724" : "#6B4654"
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            font.letterSpacing: 0.6
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: root.executablePath
                    color: root.secondaryTextColor
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                    maximumLineCount: 1
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "PID  " + root.processPid
                        color: root.secondaryTextColor
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.preferredHeight: 3
                        radius: 2
                        color: "#CAC4D0"
                    }

                    Text {
                        text: root.memoryText
                        color: root.secondaryTextColor
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }

                    Item { Layout.fillWidth: true }

                    MaterialButton {
                        Layout.preferredWidth: 94
                        Layout.preferredHeight: 38
                        text: root.sessionProcess ? "Return" : "Attach"
                        filled: false
                        foregroundColor: root.primaryColor
                        outlineColor: root.hasWindow ? root.primaryColor : "#CAC4D0"
                        enabled: root.hasWindow
                        onClicked: root.attachRequested(root.processPid)
                    }
                }
            }
        }

        MouseArea {
            id: cardMouse
            z: 0
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onPressed: function(mouse) {
                root.forceActiveFocus()
                ripple.burst(mouse.x, mouse.y)
            }
            onClicked: root.selectRequested(root.processPid)
        }

    }

    // These invisible metrics use the exact title font and allocated width.
    // Keeping the measurement separate from the displayed Text avoids a
    // circular layout dependency while the card animates between heights.
    Text {
        id: titleLineMetric
        visible: false
        text: "Ag"
        font: titleLabel.font
    }

    Text {
        id: titleExpandedMeasure
        visible: false
        width: Math.max(1, titleLabel.width)
        text: root.windowTitle
        font: titleLabel.font
        wrapMode: Text.Wrap
    }

    SequentialAnimation {
        id: entryAnimation
        PauseAnimation { duration: root.entryDelay }
        ParallelAnimation {
            NumberAnimation {
                target: root
                property: "opacity"
                from: 0
                to: 1
                duration: 420
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
            }
            NumberAnimation {
                target: entryTranslation
                property: "y"
                from: 22
                to: 0
                duration: 520
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
            }
        }
    }

    Component.onCompleted: entryAnimation.start()
}
