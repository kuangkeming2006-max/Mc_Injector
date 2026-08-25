import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import McOverlay 1.0

ApplicationWindow {
    id: app

    width: 1420
    height: 880
    minimumWidth: 1040
    minimumHeight: 680
    visible: true
    title: "Java Overlay Studio"
    color: backgroundColor

    readonly property color backgroundColor: "#F7F2FA"
    readonly property color surfaceColor: "#FFFBFE"
    readonly property color surfaceVariant: "#E7E0EC"
    readonly property color primaryColor: "#6750A4"
    readonly property color primaryContainer: "#EADDFF"
    readonly property color primaryContainerText: "#21005D"
    readonly property color textColor: "#1D1B20"
    readonly property color secondaryTextColor: "#49454F"
    readonly property color outlineColor: "#79747E"
    readonly property var menuHotkeyOptions: [
        { "label": "Apostrophe (')", "value": 222 },
        { "label": "Insert", "value": 45 },
        { "label": "Home", "value": 36 },
        { "label": "End", "value": 35 },
        { "label": "F8", "value": 119 },
        { "label": "F9", "value": 120 },
        { "label": "F10", "value": 121 },
        { "label": "F12", "value": 123 }
    ]
    // Material desktop navigation panes commonly sit around 220–240 px. Keep
    // a useful, bounded resize range instead of tying the rail to a percentage
    // of an ultrawide display. The final clamp always leaves 720 px for content.
    readonly property real defaultNavigationPaneWidth: 232
    readonly property real minimumNavigationPaneWidth: 208
    readonly property real maximumNavigationPaneWidth: 320
    property real requestedNavigationPaneWidth: AppSettings.navigationPaneWidth
    readonly property real navigationPaneWidth: Math.round(
                                                    Math.max(minimumNavigationPaneWidth,
                                                             Math.min(requestedNavigationPaneWidth,
                                                                      Math.min(maximumNavigationPaneWidth,
                                                                               width - 720))))
    readonly property bool hasSelectedProcess: ProcessScanner.selectedPid !== 0
                                               || OverlayManager.targetPid !== 0
    // Error is a first-class session state: the target and its diagnostics stay
    // visible until the user explicitly retries, browses, or detaches. Browsing
    // never tears down a healthy resident Agent.
    readonly property bool sessionAvailable: OverlayManager.attached
                                             || OverlayManager.state === OverlayManager.Error
    property bool browsingProcesses: false
    readonly property bool workspaceUnlocked: sessionAvailable && !browsingProcesses
    readonly property int activeTargetPid: OverlayManager.targetPid !== 0
                                           ? OverlayManager.targetPid
                                           : ProcessScanner.selectedPid
    readonly property string activeTargetTitle: OverlayManager.targetPid !== 0
                                                ? OverlayManager.targetTitle
                                                : (ProcessScanner.processForPid(activeTargetPid).windowTitle
                                                   || (activeTargetPid === 0 ? "" : "PID " + activeTargetPid))
    readonly property var setupNavigationItems: [
        { "icon": "⌕", "label": "Scanner", "description": "Discover Java processes", "route": "scanner" },
        { "icon": "⚙", "label": "Settings", "description": "Discovery preferences", "route": "settings" }
    ]
    // Process-specific information architecture. Hypixel is intentionally a
    // separate network tool; local world diagnostics live under ESP.
    readonly property var featureNavigationItems: [
        { "icon": "⌂", "label": "Main", "description": "Session status and controls", "route": "main" },
        { "icon": "P", "label": "Player Status", "description": "Identity, health and skin", "route": "player" },
        { "icon": "◇", "label": "ESP", "description": "Single-player diagnostics", "route": "esp" },
        { "icon": "H", "label": "Hypixel", "description": "Official API statistics", "route": "hypixel" },
        { "icon": "i", "label": "About", "description": "Build and architecture", "route": "about" },
        { "icon": "⚙", "label": "Settings", "description": "Runtime preferences", "route": "settings" }
    ]
    readonly property var navigationItems: workspaceUnlocked
                                                    ? featureNavigationItems
                                                    : setupNavigationItems

    property string activeRoute: "scanner"
    property bool autoRefresh: false
    property var pendingProcess: ({})

    function menuHotkeyIndex(virtualKey) {
        for (let index = 0; index < menuHotkeyOptions.length; ++index) {
            if (menuHotkeyOptions[index].value === virtualKey)
                return index
        }
        return 0
    }

    function menuHotkeyLabel(virtualKey) {
        for (let index = 0; index < menuHotkeyOptions.length; ++index) {
            if (menuHotkeyOptions[index].value === virtualKey)
                return menuHotkeyOptions[index].label
        }
        if ((virtualKey >= 48 && virtualKey <= 57)
                || (virtualKey >= 65 && virtualKey <= 90))
            return String.fromCharCode(virtualKey)
        return "Key " + virtualKey
    }

    onRequestedNavigationPaneWidthChanged: navigationWidthSave.restart()

    Timer {
        id: navigationWidthSave
        interval: 180
        repeat: false
        onTriggered: AppSettings.navigationPaneWidth = app.requestedNavigationPaneWidth
    }

    // This is phase progress, not a fabricated byte/percent counter. Each
    // value represents a controller milestone; WaitingForOpenGL also gets a
    // moving segment because its duration depends entirely on the next frame.
    readonly property bool injectionInProgress:
        OverlayManager.state === OverlayManager.Validating
        || OverlayManager.state === OverlayManager.StartingIpc
        || OverlayManager.state === OverlayManager.LaunchingAttachHelper
        || OverlayManager.state === OverlayManager.WaitingForAgent
        || OverlayManager.state === OverlayManager.WaitingForOpenGL
    readonly property bool waitingForOpenGL:
        OverlayManager.state === OverlayManager.WaitingForOpenGL
    readonly property real injectionPhaseProgress:
        OverlayManager.state === OverlayManager.Validating ? 0.14
        : (OverlayManager.state === OverlayManager.StartingIpc ? 0.30
           : (OverlayManager.state === OverlayManager.LaunchingAttachHelper ? 0.48
              : (OverlayManager.state === OverlayManager.WaitingForAgent ? 0.66
                 : (OverlayManager.state === OverlayManager.WaitingForOpenGL ? 0.84
                    : (OverlayManager.state === OverlayManager.Active ? 1.0 : 0.0)))))
    readonly property int injectionPhaseNumber:
        OverlayManager.state === OverlayManager.Validating ? 1
        : (OverlayManager.state === OverlayManager.StartingIpc ? 2
           : (OverlayManager.state === OverlayManager.LaunchingAttachHelper ? 3
              : (OverlayManager.state === OverlayManager.WaitingForAgent ? 4
                 : (OverlayManager.state === OverlayManager.WaitingForOpenGL ? 5
                    : (OverlayManager.state === OverlayManager.Active ? 6 : 0)))))
    readonly property string injectionPhaseLabel:
        OverlayManager.state === OverlayManager.Validating ? "Validating process"
        : (OverlayManager.state === OverlayManager.StartingIpc ? "Opening secure channel"
           : (OverlayManager.state === OverlayManager.LaunchingAttachHelper ? "Loading native agent"
              : (OverlayManager.state === OverlayManager.WaitingForAgent ? "Starting in-game runtime"
                 : (OverlayManager.state === OverlayManager.WaitingForOpenGL ? "Waiting for a game frame"
                    : (OverlayManager.state === OverlayManager.Active ? "Overlay ready" : "")))))

    function openAttachDialog(pid) {
        if (pid === OverlayManager.targetPid && app.sessionAvailable) {
            app.browsingProcesses = false
            app.activeRoute = "main"
            return
        }
        ProcessScanner.selectProcess(pid)
        const details = ProcessScanner.processForPid(pid)
        pendingProcess = {
            "pid": pid,
            "windowTitle": details.windowTitle
                           || (OverlayManager.targetPid === pid
                               ? OverlayManager.targetTitle : "Selected Java process"),
            "hasWindow": details.hasWindow === true || OverlayManager.attached
        }
        attachDialog.open()
    }

    function pageIndexForRoute(route) {
        if (route === "esp")
            return 1
        if (route === "main")
            return 2
        if (route === "hypixel")
            return 3
        if (route === "player")
            return 4
        if (route === "about")
            return 5
        if (route === "settings")
            return 6
        return 0
    }

    function gameMetric(value, decimals, suffix) {
        if (!OverlayManager.gameStateAvailable || OverlayManager.gameStateStale)
            return "—"
        return Number(value).toFixed(decimals) + (suffix || "")
    }

    function mappingStateLabel() {
        const state = (OverlayManager.mappingState || "waiting").toLowerCase()
        if (state === "ready" || state === "resolved")
            return "Mapping ready"
        if (state === "unsupported")
            return "Unsupported mappings"
        if (state === "resolving")
            return "Resolving mappings"
        if (state === "waiting_for_game_thread")
            return "Waiting for game thread"
        if (state === "no_player")
            return "Waiting for local player"
        if (state === "jni_error")
            return "JNI data read unavailable"
        if (state === "unavailable")
            return "Game data unavailable"
        return "Waiting for game data"
    }

    function gameStateSummary() {
        if (!OverlayManager.attached)
            return "Attach the native agent to start the live data channel."
        if (!OverlayManager.gameStateReceived)
            return "Secure IPC connected · waiting for the first game snapshot."
        if (OverlayManager.gameStateStale)
            return "Telemetry has paused. Values are hidden until a fresh snapshot arrives."
        const state = (OverlayManager.mappingState || "").toLowerCase()
        if (state === "unsupported")
            return "Overlay rendering is active, but this client needs a compatible mapping profile."
        if (!OverlayManager.gameStateAvailable)
            return "The mapping resolver is still preparing Minecraft game data."
        return "Live JNI snapshot · sequence " + OverlayManager.gameStateSequence
    }

    function sampleTimeLabel() {
        if (!OverlayManager.gameStateReceived || OverlayManager.gameStateTimestamp <= 0)
            return "No samples yet"
        return "Agent time " + new Date(Number(OverlayManager.gameStateTimestamp)).toLocaleTimeString()
    }

    function browseProcesses() {
        browsingProcesses = true
        activeRoute = "scanner"
        ProcessScanner.refresh()
    }

    function returnToSession() {
        browsingProcesses = false
        activeRoute = "main"
    }

    function clearScannerSelection() {
        if (app.injectionInProgress)
            OverlayManager.detach()
        pendingProcess = ({})
        ProcessScanner.selectProcess(0)
        ProcessScanner.refresh()
    }

    Connections {
        target: ProcessScanner

        function onSelectedPidChanged() {
            if (!app.hasSelectedProcess)
                app.activeRoute = "scanner"
        }
    }

    Connections {
        target: OverlayManager

        function onTargetExited() {
            app.browsingProcesses = false
            app.activeRoute = "scanner"
        }

        function onStateChanged() {
            if (OverlayManager.state === OverlayManager.WaitingForOpenGL) {
                // HELLO has been authenticated: reveal the process workspace
                // and lead with its live-data overview while OpenGL starts.
                app.browsingProcesses = false
                app.activeRoute = "main"
            } else if (OverlayManager.state === OverlayManager.Active) {
                app.browsingProcesses = false
                app.activeRoute = "main"
                injectionSnackbar.open()
                snackbarCloseTimer.restart()
            } else if (OverlayManager.state === OverlayManager.Error) {
                app.browsingProcesses = false
                app.activeRoute = "main"
                snackbarCloseTimer.stop()
                injectionSnackbar.close()
            } else if (OverlayManager.state === OverlayManager.Detached) {
                app.browsingProcesses = false
                app.activeRoute = "scanner"
                snackbarCloseTimer.stop()
                injectionSnackbar.close()
            }
        }
    }

    Timer {
        interval: 5000
        repeat: true
        running: app.autoRefresh
        onTriggered: ProcessScanner.refresh()
    }

    Rectangle {
        anchors.fill: parent
        color: app.backgroundColor
    }

    Rectangle {
        id: navigationRail
        width: app.navigationPaneWidth
        z: 2
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        color: "#F1ECF4"

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 22
            anchors.rightMargin: 22
            anchors.topMargin: 26
            anchors.bottomMargin: 24
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                Rectangle {
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 56
                    radius: 18
                    color: app.primaryColor

                    Text {
                        anchors.centerIn: parent
                        text: "J"
                        color: "white"
                        font.pixelSize: 25
                        font.weight: Font.Bold
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: "JAVA OVERLAY"
                        color: app.textColor
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: app.workspaceUnlocked ? "Process workspace" : "Process setup"
                        color: app.secondaryTextColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: "#DDD6E1"
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 14

                Text {
                    anchors.fill: parent
                    text: "SETUP"
                    opacity: app.workspaceUnlocked ? 0 : 1
                    color: app.secondaryTextColor
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    font.letterSpacing: 1.2
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                }
                Text {
                    anchors.fill: parent
                    text: "PROCESS TOOLS"
                    opacity: app.workspaceUnlocked ? 1 : 0
                    color: app.secondaryTextColor
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    font.letterSpacing: 1.2
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 300
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                        }
                    }
                }
            }

            ListView {
                id: navigationList
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: app.navigationItems
                spacing: 8
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                add: Transition {
                    ParallelAnimation {
                        NumberAnimation {
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: 300
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                        }
                        NumberAnimation {
                            property: "x"
                            from: -16
                            to: 0
                            duration: 360
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                        }
                    }
                }
                remove: Transition {
                    ParallelAnimation {
                        NumberAnimation {
                            property: "opacity"
                            to: 0
                            duration: 150
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
                        }
                        NumberAnimation {
                            property: "x"
                            to: -10
                            duration: 180
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar {
                    policy: navigationList.contentHeight > navigationList.height
                            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }

                delegate: Item {
                    required property int index
                    required property var modelData
                    width: navigationList.width
                    height: 68
                    opacity: 0
                    transform: Translate { id: navigationEntryOffset; x: -12 }

                    SequentialAnimation on opacity {
                        running: true
                        PauseAnimation { duration: index * 45 }
                        NumberAnimation {
                            from: 0
                            to: 1
                            duration: 280
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                        }
                    }
                    SequentialAnimation {
                        running: true
                        PauseAnimation { duration: index * 45 }
                        NumberAnimation {
                            target: navigationEntryOffset
                            property: "x"
                            from: -12
                            to: 0
                            duration: 340
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                        }
                    }

                    Rectangle {
                        id: navPill
                        anchors.fill: parent
                        radius: 20
                        // Selection and hover are separate layers. Animating a
                        // color binding that alternated between transparent,
                        // hover and selected colors caused two successive
                        // darkening steps when the delegate state changed.
                        color: app.activeRoute === modelData.route
                               ? app.primaryContainer : "transparent"
                        scale: navMouse.pressed ? 0.985 : 1
                        transformOrigin: Item.Center

                        Behavior on color { ColorAnimation { duration: 180 } }
                        Behavior on scale {
                            NumberAnimation {
                                duration: 240
                                easing.type: Easing.BezierSpline
                                easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                            }
                        }

                        RippleEffect {
                            id: navRipple
                            anchors.fill: parent
                            rippleColor: app.primaryColor
                            peakOpacity: 0.13
                            cornerRadius: navPill.radius
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: "#E3DDE7"
                            // Keep the hover wash translucent so the clipped ripple remains
                            // visible.  A single animated layer avoids the previous two-step
                            // darkening caused by animating both the pill and its overlay.
                            opacity: navMouse.containsMouse
                                     && app.activeRoute !== modelData.route ? 0.72 : 0
                            Behavior on opacity { NumberAnimation { duration: 160 } }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 14

                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                radius: 13
                                color: app.activeRoute === modelData.route
                                       ? "#D7C7F5" : "#E7E0EC"

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.icon
                                    color: app.activeRoute === modelData.route
                                           ? app.primaryContainerText : app.secondaryTextColor
                                    font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    color: app.activeRoute === modelData.route
                                           ? app.primaryContainerText : app.textColor
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.description
                                    color: app.secondaryTextColor
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: navMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onPressed: function(mouse) {
                            const point = mapToItem(navPill, mouse.x, mouse.y)
                            navRipple.burst(point.x, point.y)
                        }
                        onClicked: app.activeRoute = modelData.route
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: app.hasSelectedProcess ? 148 : 94
                radius: 24
                color: app.hasSelectedProcess ? "#FFFBFE" : "#E7E0EC"
                border.width: app.hasSelectedProcess ? 1 : 0
                border.color: "#DED8E2"

                Behavior on Layout.preferredHeight {
                    NumberAnimation {
                        duration: 360
                        easing.type: Easing.BezierSpline
                        easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 7
                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            color: OverlayManager.rendererActive ? "#20853B"
                                  : (OverlayManager.attached || OverlayManager.busy
                                     ? "#B77900"
                                     : (app.hasSelectedProcess ? app.primaryColor : "#79747E"))
                        }
                        Text {
                            Layout.fillWidth: true
                            text: OverlayManager.rendererActive ? "OVERLAY LIVE"
                                  : (OverlayManager.attached ? "AGENT READY"
                                     : (OverlayManager.busy ? "ATTACHING"
                                        : (app.hasSelectedProcess ? "PROCESS SELECTED" : "READY TO SCAN")))
                            color: OverlayManager.rendererActive ? "#155724" : app.secondaryTextColor
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            font.letterSpacing: 0.7
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: app.hasSelectedProcess
                              ? app.activeTargetTitle
                              : "Select a Java process to unlock its tools."
                        color: app.textColor
                        font.pixelSize: app.hasSelectedProcess ? 14 : 12
                        font.weight: app.hasSelectedProcess ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.hasSelectedProcess
                        Text {
                            Layout.fillWidth: true
                            text: "PID " + app.activeTargetPid
                            color: app.secondaryTextColor
                            font.pixelSize: 11
                        }
                        MaterialButton {
                            Layout.preferredWidth: 116
                            Layout.preferredHeight: 36
                            text: app.sessionAvailable
                                  ? (app.browsingProcesses ? "Return" : "Browse processes")
                                  : "Clear selection"
                            filled: false
                            foregroundColor: app.primaryColor
                            outlineColor: "transparent"
                            onClicked: {
                                if (app.sessionAvailable) {
                                    if (app.browsingProcesses)
                                        app.returnToSession()
                                    else
                                        app.browseProcesses()
                                } else {
                                    app.clearScannerSelection()
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: "#DDD6E1"
        }

        // A 12 px hit area straddles the visual divider, so the resize target
        // remains easy to acquire without making the divider look heavy.
        MouseArea {
            id: navigationResizeHandle
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.rightMargin: -6
            width: 12
            z: 20
            hoverEnabled: true
            cursorShape: Qt.SizeHorCursor
            preventStealing: true

            property real pressWindowX: 0
            property real pressPaneWidth: app.requestedNavigationPaneWidth

            onPressed: function(mouse) {
                pressWindowX = mapToItem(app.contentItem, mouse.x, mouse.y).x
                pressPaneWidth = app.navigationPaneWidth
            }
            onPositionChanged: function(mouse) {
                if (!pressed)
                    return
                const windowX = mapToItem(app.contentItem, mouse.x, mouse.y).x
                app.requestedNavigationPaneWidth = Math.max(
                            app.minimumNavigationPaneWidth,
                            Math.min(app.maximumNavigationPaneWidth,
                                     pressPaneWidth + windowX - pressWindowX))
            }
            onReleased: AppSettings.navigationPaneWidth = app.requestedNavigationPaneWidth
            onCanceled: AppSettings.navigationPaneWidth = app.requestedNavigationPaneWidth
            onDoubleClicked: app.requestedNavigationPaneWidth
                             = app.defaultNavigationPaneWidth

            ToolTip.visible: containsMouse
            ToolTip.delay: 500
            ToolTip.text: "Drag to resize · Double-click to reset"

            Rectangle {
                anchors.centerIn: parent
                width: navigationResizeHandle.pressed ? 4 : 3
                height: navigationResizeHandle.containsMouse ? 52 : 36
                radius: width / 2
                color: navigationResizeHandle.containsMouse
                       ? app.primaryColor : "#9A949E"
                opacity: navigationResizeHandle.containsMouse ? 0.9 : 0.45

                Behavior on height {
                    NumberAnimation {
                        duration: 260
                        easing.type: Easing.BezierSpline
                        easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                    }
                }
                Behavior on color { ColorAnimation { duration: 160 } }
                Behavior on opacity { NumberAnimation { duration: 160 } }
            }
        }
    }

    Item {
        id: workspace
        anchors.left: navigationRail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: actionBar.top

        StackLayout {
            anchors.fill: parent
            currentIndex: app.pageIndexForRoute(app.activeRoute)

            // Process Scanner -------------------------------------------------
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 34
                    anchors.rightMargin: 24
                    anchors.topMargin: 28
                    spacing: 20

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Text {
                                text: "Process scanner"
                                color: app.textColor
                                font.pixelSize: 32
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: "Choose a running Java or Minecraft window to host the overlay."
                                color: app.secondaryTextColor
                                font.pixelSize: 14
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: scanStatus.implicitWidth + 28
                            Layout.preferredHeight: 38
                            visible: workspace.width >= 820
                            radius: 19
                            color: "#EEE8F1"

                            Row {
                                anchors.centerIn: parent
                                spacing: 8
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 8
                                    height: 8
                                    radius: 4
                                    color: ProcessScanner.refreshing ? "#F2A000" : app.primaryColor

                                    SequentialAnimation on opacity {
                                        running: ProcessScanner.refreshing
                                        loops: Animation.Infinite
                                        NumberAnimation { to: 0.25; duration: 520 }
                                        NumberAnimation { to: 1; duration: 520 }
                                    }
                                }
                                Text {
                                    id: scanStatus
                                    text: ProcessScanner.statusMessage
                                    color: app.secondaryTextColor
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                            }
                        }

                        MaterialButton {
                            text: ProcessScanner.refreshing ? "Scanning…" : "Refresh"
                            iconText: "↻"
                            filled: false
                            visible: workspace.width >= 720
                            foregroundColor: app.primaryColor
                            outlineColor: app.outlineColor
                            enabled: !ProcessScanner.refreshing
                            onClicked: ProcessScanner.refresh()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 24
                        color: "#ECE6EF"
                        clip: true

                        Flickable {
                            id: processGrid
                            anchors.fill: parent
                            anchors.margins: 18
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            property int columnCount: width >= 1080 ? 3 : (width >= 680 ? 2 : 1)
                            contentWidth: width
                            contentHeight: processCardLayout.implicitHeight

                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded
                            }

                            // GridView assumes one fixed cell height and would
                            // overlap a card after its full title expands.
                            // GridLayout keeps wide-screen columns while each
                            // row naturally adopts its tallest card.
                            GridLayout {
                                id: processCardLayout
                                width: processGrid.width
                                columns: processGrid.columnCount
                                columnSpacing: 18
                                rowSpacing: 18

                                Repeater {
                                    model: ProcessScanner

                                    delegate: ProcessCard {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignTop
                                        Layout.preferredWidth: Math.floor(
                                                                   (processCardLayout.width
                                                                    - processCardLayout.columnSpacing
                                                                      * (processCardLayout.columns - 1))
                                                                   / processCardLayout.columns)
                                        Layout.minimumWidth: Layout.preferredWidth
                                        Layout.maximumWidth: Layout.preferredWidth
                                        Layout.preferredHeight: implicitHeight
                                        height: implicitHeight
                                        processPid: model.pid
                                        executableName: model.executableName
                                        executablePath: model.executablePath
                                        windowTitle: model.windowTitle
                                        memoryText: model.memoryText
                                        hasWindow: model.hasWindow
                                        selected: model.selected
                                        entryDelay: Math.min(index, 8) * 55
                                        primaryColor: app.primaryColor
                                        surfaceColor: app.surfaceColor
                                        textColor: app.textColor
                                        secondaryTextColor: app.secondaryTextColor
                                        sessionProcess: processPid === OverlayManager.targetPid
                                                        && app.sessionAvailable
                                        onSelectRequested: function(pid) { ProcessScanner.selectProcess(pid) }
                                        onAttachRequested: function(pid) { app.openAttachDialog(pid) }
                                    }
                                }
                            }
                        }

                        Column {
                            anchors.centerIn: parent
                            spacing: 12
                            visible: ProcessScanner.count === 0 && !ProcessScanner.refreshing

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 68
                                height: 68
                                radius: 24
                                color: app.primaryContainer
                                Text {
                                    anchors.centerIn: parent
                                    text: "J?"
                                    color: app.primaryContainerText
                                    font.pixelSize: 22
                                    font.weight: Font.Bold
                                }
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "No Java processes are running"
                                color: app.textColor
                                font.pixelSize: 18
                                font.weight: Font.DemiBold
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Start Minecraft, then refresh the scanner."
                                color: app.secondaryTextColor
                                font.pixelSize: 13
                            }
                        }
                    }
                }
            }

            // ESP / local world diagnostics --------------------------------
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: mainContent.implicitHeight + 58
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    ScrollBar.vertical: ScrollBar {
                        policy: mainContent.implicitHeight + 58 > parent.height
                                ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                    }

                    ColumnLayout {
                        id: mainContent
                        x: 34
                        y: 28
                        width: parent.width - 68
                        spacing: 18

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text {
                                    text: "ESP"
                                    color: app.textColor
                                    font.pixelSize: 32
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "Entity hitboxes and bed markers for single-player world debugging"
                                    color: app.secondaryTextColor
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                }
                            }

                            Rectangle {
                                Layout.preferredWidth: liveStateLabel.implicitWidth + 34
                                Layout.preferredHeight: 38
                                radius: 19
                                color: !OverlayManager.gameStateReceived ? "#E7E0EC"
                                     : (OverlayManager.gameStateStale ? "#FFE2E0"
                                        : (OverlayManager.gameStateAvailable ? "#D7F7DD" : "#FFF1C7"))

                                Row {
                                    anchors.centerIn: parent
                                    spacing: 8
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 8
                                        height: 8
                                        radius: 4
                                        color: !OverlayManager.gameStateReceived ? "#79747E"
                                             : (OverlayManager.gameStateStale ? "#BA1A1A"
                                                : (OverlayManager.gameStateAvailable ? "#20853B" : "#B77900"))
                                    }
                                    Text {
                                        id: liveStateLabel
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: !OverlayManager.gameStateReceived ? "WAITING"
                                            : (OverlayManager.gameStateStale ? "STALE"
                                               : (OverlayManager.gameStateAvailable ? "LIVE" : "MAPPING"))
                                        color: app.textColor
                                        font.pixelSize: 10
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.8
                                    }
                                }

                                Behavior on color { ColorAnimation { duration: 220 } }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 94
                            radius: 24
                            color: !OverlayManager.gameStateReceived ? app.surfaceVariant
                                 : (OverlayManager.gameStateStale ? "#FFF0EF"
                                    : ((OverlayManager.mappingState || "").toLowerCase() === "unsupported"
                                       ? "#FFF1C7" : app.primaryContainer))

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 15

                                Rectangle {
                                    Layout.preferredWidth: 50
                                    Layout.preferredHeight: 50
                                    radius: 17
                                    color: OverlayManager.gameStateAvailable
                                           && !OverlayManager.gameStateStale
                                           ? app.primaryColor : "#D6CFDA"
                                    Text {
                                        anchors.centerIn: parent
                                        text: OverlayManager.gameStateAvailable
                                              && !OverlayManager.gameStateStale ? "↯" : "…"
                                        color: OverlayManager.gameStateAvailable
                                               && !OverlayManager.gameStateStale ? "white" : app.secondaryTextColor
                                        font.pixelSize: 22
                                        font.weight: Font.Bold
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        Layout.fillWidth: true
                                        text: app.mappingStateLabel()
                                        color: app.textColor
                                        font.pixelSize: 17
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: app.gameStateSummary()
                                        color: app.secondaryTextColor
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Text {
                                    visible: OverlayManager.gameStateReceived
                                    text: app.sampleTimeLabel()
                                    color: app.secondaryTextColor
                                    font.pixelSize: 10
                                }
                            }

                            Behavior on color { ColorAnimation { duration: 240 } }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: featureControls.implicitHeight + 38
                            radius: 24
                            color: app.surfaceColor
                            border.width: 1
                            border.color: "#E7E0EC"

                            ColumnLayout {
                                id: featureControls
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 19
                                spacing: 10

                                Text {
                                    text: "IN-GAME FEATURE CONTROLS"
                                    color: app.secondaryTextColor
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.9
                                }

                                Repeater {
                                    model: [
                                        { "label": "ESP master", "detail": "Single-player world-space diagnostics", "key": "master" },
                                        { "label": "3D living hitboxes", "detail": "Occlusion-independent projected AABB wireframes", "key": "entities" },
                                        { "label": "Bed ESP", "detail": "Chunk diffing, bulk section copy and lazy verification", "key": "beds" },
                                        { "label": "Bed proximity alert", "detail": "Rate-limited lower-right warning when another player enters 8 blocks", "key": "bedThreat" },
                                        { "label": "Bed defense panel", "detail": "Fixed-size material icons above each detected bed", "key": "bedDefense" },
                                        { "label": "World labels", "detail": "Coordinates and entity identifiers", "key": "labels" },
                                        { "label": "Hypixel panel", "detail": "Show the draggable official-API result card in game", "key": "hypixel" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 14
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1
                                            Text { text: modelData.label; color: app.textColor; font.pixelSize: 14; font.weight: Font.Medium }
                                            Text { text: modelData.detail; color: app.secondaryTextColor; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                                        }
                                        Switch {
                                            enabled: OverlayManager.attached
                                            checked: modelData.key === "master" ? OverlayManager.espEnabled
                                                   : modelData.key === "entities" ? OverlayManager.entityEspEnabled
                                                   : modelData.key === "beds" ? OverlayManager.bedEspEnabled
                                                   : modelData.key === "bedThreat" ? OverlayManager.bedThreatAlertsEnabled
                                                   : modelData.key === "bedDefense" ? OverlayManager.bedDefensePanelEnabled
                                                   : modelData.key === "labels" ? OverlayManager.espLabelsEnabled
                                                   : OverlayManager.hypixelPanelEnabled
                                            onToggled: {
                                                if (modelData.key === "master") OverlayManager.espEnabled = checked
                                                else if (modelData.key === "entities") OverlayManager.entityEspEnabled = checked
                                                else if (modelData.key === "beds") OverlayManager.bedEspEnabled = checked
                                                else if (modelData.key === "bedThreat") OverlayManager.bedThreatAlertsEnabled = checked
                                                else if (modelData.key === "bedDefense") OverlayManager.bedDefensePanelEnabled = checked
                                                else if (modelData.key === "labels") OverlayManager.espLabelsEnabled = checked
                                                else OverlayManager.hypixelPanelEnabled = checked
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Text {
                                        Layout.fillWidth: true
                                        text: "Bed material radius"
                                        color: app.textColor
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                    }
                                    Repeater {
                                        model: 8
                                        MaterialButton {
                                            required property int index
                                            readonly property int radiusValue: index + 3
                                            compact: true
                                            text: radiusValue.toString()
                                            filled: OverlayManager.bedDefenseRadius === radiusValue
                                            containerColor: app.primaryColor
                                            foregroundColor: filled ? "white" : app.primaryColor
                                            outlineColor: "#CAC4D0"
                                            enabled: OverlayManager.attached
                                            onClicked: OverlayManager.bedDefenseRadius = radiusValue
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Text {
                                        Layout.fillWidth: true
                                        text: "Placed a bed in an already loaded chunk? Rebuild the bed cache immediately."
                                        color: app.secondaryTextColor
                                        font.pixelSize: 11
                                        wrapMode: Text.WordWrap
                                    }
                                    MaterialButton {
                                        Layout.preferredWidth: 150
                                        Layout.preferredHeight: 40
                                        text: "Refresh beds now"
                                        filled: false
                                        enabled: OverlayManager.attached
                                        foregroundColor: app.primaryColor
                                        outlineColor: "#CAC4D0"
                                        onClicked: OverlayManager.refreshBedCache()
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Press " + app.menuHotkeyLabel(OverlayManager.menuHotkey)
                                          + " in Minecraft to open the animated Click GUI. ESP remains hard-locked outside integrated single-player worlds."
                                    color: app.primaryColor
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: width >= 980 ? 4 : 2
                            columnSpacing: 16
                            rowSpacing: 16

                            Repeater {
                                model: [
                                    {
                                        "label": "HEALTH",
                                        "value": app.gameMetric(OverlayManager.playerHealth, 1, " HP"),
                                        "detail": OverlayManager.gameStateAvailable && !OverlayManager.gameStateStale
                                                  ? "of " + Number(OverlayManager.playerMaxHealth).toFixed(1) + " HP" : "Awaiting player",
                                        "icon": "♥",
                                        "tint": "#FFE2E0"
                                    },
                                    {
                                        "label": "ENTITY ID",
                                        "value": OverlayManager.gameStateAvailable && !OverlayManager.gameStateStale
                                                 ? "#" + OverlayManager.playerEntityId : "—",
                                        "detail": "Local player",
                                        "icon": "ID",
                                        "tint": "#EADDFF"
                                    },
                                    {
                                        "label": "ENTITIES",
                                        "value": OverlayManager.gameStateAvailable && !OverlayManager.gameStateStale
                                                 ? String(OverlayManager.loadedEntities) : "—",
                                        "detail": "Loaded in world",
                                        "icon": "◎",
                                        "tint": "#D7F7DD"
                                    },
                                    {
                                        "label": "BEDS",
                                        "value": OverlayManager.gameStateAvailable && !OverlayManager.gameStateStale
                                                 ? String(OverlayManager.bedCount) : "—",
                                        "detail": "Known loaded blocks",
                                        "icon": "▰",
                                        "tint": "#FFF1C7"
                                    }
                                ]

                                delegate: Rectangle {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 154
                                    radius: 22
                                    color: metricHover.hovered ? "#FFFFFF" : app.surfaceColor
                                    border.width: 1
                                    border.color: metricHover.hovered ? "#C9BED1" : "#E7E0EC"
                                    scale: metricHover.hovered ? 1.012 : 1

                                    HoverHandler { id: metricHover }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        spacing: 7

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData.label
                                                color: app.secondaryTextColor
                                                font.pixelSize: 10
                                                font.weight: Font.Bold
                                                font.letterSpacing: 0.9
                                            }
                                            Rectangle {
                                                Layout.preferredWidth: 38
                                                Layout.preferredHeight: 38
                                                radius: 13
                                                color: modelData.tint
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: modelData.icon
                                                    color: app.textColor
                                                    font.pixelSize: modelData.icon === "ID" ? 10 : 18
                                                    font.weight: Font.Bold
                                                }
                                            }
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.value
                                            color: app.textColor
                                            font.pixelSize: 24
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.detail
                                            color: app.secondaryTextColor
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Behavior on scale {
                                        NumberAnimation {
                                            duration: 260
                                            easing.type: Easing.BezierSpline
                                            easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                                        }
                                    }
                                    Behavior on color { ColorAnimation { duration: 180 } }
                                    Behavior on border.color { ColorAnimation { duration: 180 } }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 3
                                Layout.preferredHeight: 132
                                radius: 22
                                color: app.surfaceColor
                                border.width: 1
                                border.color: "#E7E0EC"

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 20
                                    spacing: 8
                                    Text {
                                        text: "PLAYER POSITION"
                                        color: app.secondaryTextColor
                                        font.pixelSize: 10
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.9
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "X  " + app.gameMetric(OverlayManager.playerX, 2, "")
                                              + "     Y  " + app.gameMetric(OverlayManager.playerY, 2, "")
                                              + "     Z  " + app.gameMetric(OverlayManager.playerZ, 2, "")
                                        color: app.textColor
                                        font.pixelSize: width >= 620 ? 21 : 16
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: "World-space coordinates from the local player binding"
                                        color: app.secondaryTextColor
                                        font.pixelSize: 11
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 2
                                Layout.preferredHeight: 132
                                radius: 22
                                color: "#211E24"

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 20
                                    spacing: 7
                                    Text {
                                        text: "MAPPING PROFILE"
                                        color: "#CAC4D0"
                                        font.pixelSize: 10
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.9
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: OverlayManager.mappingProfile.length > 0
                                              ? OverlayManager.mappingProfile : "Not detected"
                                        color: "#FFFBFE"
                                        font.pixelSize: 18
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: app.mappingStateLabel()
                                        color: "#D0BCFF"
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }

                    }
                }
            }

            // Main / session dashboard ---------------------------------------
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 34
                    spacing: 20

                    Text {
                        text: "Main"
                        color: app.textColor
                        font.pixelSize: 32
                        font.weight: Font.DemiBold
                    }
                    Text {
                            text: "Review the active process, injection progress, errors, and overlay controls."
                        color: app.secondaryTextColor
                        font.pixelSize: 14
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 20

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 2
                            Layout.minimumWidth: 520
                            radius: 24
                            color: app.surfaceColor
                            border.width: 1
                            border.color: "#E7E0EC"

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 26
                                spacing: 18

                                RowLayout {
                                    Layout.fillWidth: true
                                    Rectangle {
                                        Layout.preferredWidth: 60
                                        Layout.preferredHeight: 60
                                        radius: 20
                                        color: OverlayManager.rendererActive ? "#D7F7DD"
                                              : (OverlayManager.attached || OverlayManager.busy
                                                 ? "#FFF1C7" : app.surfaceVariant)
                                        Text {
                                            anchors.centerIn: parent
                                            text: OverlayManager.rendererActive ? "✓"
                                                  : (OverlayManager.attached || OverlayManager.busy ? "…" : "—")
                                            color: OverlayManager.rendererActive ? "#155724"
                                                  : (OverlayManager.attached || OverlayManager.busy
                                                     ? "#7A4F00" : app.outlineColor)
                                            font.pixelSize: 25
                                            font.weight: Font.Bold
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            Layout.fillWidth: true
                                            text: app.activeTargetPid !== 0
                                                  ? app.activeTargetTitle : "No active target"
                                            color: app.textColor
                                            font.pixelSize: 20
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: app.activeTargetPid !== 0
                                                  ? "PID " + app.activeTargetPid
                                                  : "Attach from the scanner to begin"
                                            color: app.secondaryTextColor
                                            font.pixelSize: 13
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: "#E7E0EC"
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text {
                                        Layout.fillWidth: true
                                        text: OverlayManager.rendererActive
                                              ? "In-game renderer active"
                                              : (OverlayManager.attached ? "Waiting for an OpenGL frame"
                                                 : (OverlayManager.busy ? "Loading native agent…" : "Agent not connected"))
                                        color: OverlayManager.rendererActive ? "#155724" : app.secondaryTextColor
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: OverlayManager.statusMessage
                                        color: app.secondaryTextColor
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 5
                                        spacing: 7
                                        visible: app.injectionInProgress

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Text {
                                                Layout.fillWidth: true
                                                text: app.injectionPhaseLabel
                                                color: app.primaryColor
                                                font.pixelSize: 11
                                                font.weight: Font.DemiBold
                                            }
                                            Text {
                                                text: "PHASE " + app.injectionPhaseNumber
                                                color: app.outlineColor
                                                font.pixelSize: 9
                                                font.weight: Font.Bold
                                                font.letterSpacing: 0.8
                                            }
                                        }

                                        Rectangle {
                                            id: injectionProgressTrack
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 6
                                            radius: 3
                                            color: "#E6DFEA"
                                            clip: true

                                            Rectangle {
                                                width: injectionProgressTrack.width
                                                       * app.injectionPhaseProgress
                                                height: parent.height
                                                radius: parent.radius
                                                color: app.primaryColor

                                                Behavior on width {
                                                    NumberAnimation {
                                                        duration: 420
                                                        easing.type: Easing.BezierSpline
                                                        easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                                                    }
                                                }
                                            }

                                            // The last phase has no meaningful
                                            // percentage: Minecraft controls
                                            // when the next SwapBuffers arrives.
                                            Rectangle {
                                                id: openGlProgressSweep
                                                visible: app.waitingForOpenGL
                                                width: Math.max(58, injectionProgressTrack.width * 0.22)
                                                height: parent.height
                                                radius: parent.radius
                                                color: "#D0BCFF"

                                                NumberAnimation on x {
                                                    running: openGlProgressSweep.visible
                                                    loops: Animation.Infinite
                                                    from: -openGlProgressSweep.width
                                                    to: injectionProgressTrack.width
                                                    duration: 1250
                                                    easing.type: Easing.BezierSpline
                                                    easing.bezierCurve: [0.2, 0.0, 0.0, 1.0, 1.0, 1.0]
                                                }
                                            }
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: OverlayManager.renderer.length > 0
                                        text: "Renderer: " + OverlayManager.renderer
                                        color: app.primaryColor
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: OverlayManager.errorDetail.length > 0
                                        text: OverlayManager.errorCode + ": " + OverlayManager.errorDetail
                                        color: "#BA1A1A"
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text { text: "Overlay visible"; color: app.textColor; font.pixelSize: 16; font.weight: Font.Medium }
                                        Text { text: "Render Dear ImGui before Minecraft swaps its OpenGL frame"; color: app.secondaryTextColor; font.pixelSize: 12 }
                                    }
                                    Switch {
                                        checked: OverlayManager.overlayEnabled
                                        enabled: OverlayManager.attached
                                        onToggled: OverlayManager.overlayEnabled = checked
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text { text: "Interactive mode"; color: app.textColor; font.pixelSize: 16; font.weight: Font.Medium }
                                        Text { text: "Capture input in ImGui; turn off to pass all input to the game"; color: app.secondaryTextColor; font.pixelSize: 12 }
                                    }
                                    Switch {
                                        checked: OverlayManager.interactive
                                        enabled: OverlayManager.attached
                                        onToggled: OverlayManager.interactive = checked
                                    }
                                }

                                Item { Layout.fillHeight: true }

                                MaterialButton {
                                    Layout.alignment: Qt.AlignLeft
                                    text: "Retry attach"
                                    iconText: "↻"
                                    visible: OverlayManager.state === OverlayManager.Error
                                    filled: true
                                    containerColor: app.primaryColor
                                    enabled: OverlayManager.targetPid !== 0 && !OverlayManager.busy
                                    onClicked: OverlayManager.attachToProcess(OverlayManager.targetPid)
                                }

                                MaterialButton {
                                    Layout.alignment: Qt.AlignLeft
                                    text: "Detach overlay"
                                    iconText: "×"
                                    filled: false
                                    foregroundColor: "#BA1A1A"
                                    outlineColor: "#BA1A1A"
                                    enabled: OverlayManager.attached
                                    visible: OverlayManager.attached
                                    onClicked: OverlayManager.detach()
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 300
                            visible: workspace.width >= 940
                            radius: 24
                            color: "#211E24"
                            clip: true

                            ColumnLayout {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 22
                                spacing: 16

                                Text {
                                    Layout.fillWidth: true
                                    text: "LIVE SURFACE PREVIEW"
                                    color: "#D0C8D7"
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                    font.letterSpacing: 1.1
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 128
                                    radius: 20
                                    color: "#E91E1B20"
                                    border.width: 1
                                    border.color: "#45FFFFFF"

                                    Column {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6
                                        Text { text: "JNI NATIVE OVERLAY"; color: "#D0BCFF"; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 1 }
                                        Text { text: OverlayManager.rendererActive ? "In-game renderer active" : "Preview"; color: "white"; font.pixelSize: 19; font.weight: Font.DemiBold }
                                        Text { text: OverlayManager.interactive ? "ImGui input capture enabled" : "Game input passthrough"; color: "#CAC4D0"; font.pixelSize: 12 }
                                    }
                                }
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                width: 2
                                height: 34
                                color: "#90D0BCFF"
                            }
                            Rectangle {
                                anchors.centerIn: parent
                                width: 34
                                height: 2
                                color: "#90D0BCFF"
                            }
                        }
                    }
                }
            }

            // Hypixel official API -------------------------------------------
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: hypixelContent.implicitHeight + 68
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    ColumnLayout {
                        id: hypixelContent
                        x: 34
                        y: 28
                        width: parent.width - 68
                        spacing: 18

                        Text {
                            text: "Hypixel"
                            color: app.textColor
                            font.pixelSize: 32
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Bed Wars statistics queried asynchronously through the official Hypixel API"
                            color: app.secondaryTextColor
                            font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 78
                            radius: 20
                            color: OverlayManager.matchActive ? "#D9F8DF" : "#F1ECF3"

                            Behavior on color { ColorAnimation { duration: 220 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 20
                                anchors.rightMargin: 20
                                spacing: 14
                                Rectangle {
                                    Layout.preferredWidth: 38
                                    Layout.preferredHeight: 38
                                    radius: 13
                                    color: OverlayManager.matchActive ? "#20853B" : "#79747E"
                                    Behavior on color { ColorAnimation { duration: 220 } }
                                    Text {
                                        anchors.centerIn: parent
                                        text: OverlayManager.matchActive ? "✓" : "Ⅱ"
                                        color: "white"
                                        font.pixelSize: 17
                                        font.weight: Font.Bold
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        text: "Automatic match lookup"
                                        color: app.textColor
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: OverlayManager.matchActive
                                              ? "Bed Wars team roster detected · queued players are queried once"
                                              : "Paused outside a Bed Wars match · no automatic API traffic"
                                        color: app.secondaryTextColor
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                    }
                                }
                                Text {
                                    text: ApiKeys.configured ? "KEY READY" : "KEY REQUIRED"
                                    color: ApiKeys.configured ? "#20853B" : "#BA1A1A"
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.7
                                }
                            }
                        }
                        HypixelStatsCard {
                            Layout.fillWidth: true
                            primaryColor: app.primaryColor
                            textColor: app.textColor
                            secondaryTextColor: app.secondaryTextColor
                            surfaceColor: app.surfaceColor
                            outlineColor: app.outlineColor
                        }
                    }
                }
            }

            // Player identity / skin ----------------------------------------
            PlayerStatusPage {
                surfaceColor: app.surfaceColor
                textColor: app.textColor
                secondaryTextColor: app.secondaryTextColor
                primaryColor: app.primaryColor
            }

            // About ---------------------------------------------------------
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: aboutContent.implicitHeight + 68
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: aboutContent
                        x: 34; y: 28; width: parent.width - 68; spacing: 18
                        Text { text: "About"; color: app.textColor; font.pixelSize: 32; font.weight: Font.DemiBold }
                        Text { text: "Java Overlay Studio · Debug v12"; color: app.secondaryTextColor; font.pixelSize: 14 }

                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 210
                            radius: 26; color: "#EADDFF"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 28; spacing: 24
                                Rectangle {
                                    Layout.preferredWidth: 76; Layout.preferredHeight: 76; radius: 25; color: app.primaryColor
                                    Text { anchors.centerIn: parent; text: "MC"; color: "white"; font.pixelSize: 20; font.weight: Font.Bold }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 8
                                    Text { text: "Native Minecraft 1.8.9 Overlay"; color: app.primaryContainerText; font.pixelSize: 23; font.weight: Font.DemiBold }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "C++20 · Qt 6/QML · JVMTI/JNI · Dear ImGui · OpenGL 2 · authenticated bidirectional IPC"
                                        color: "#4F378B"; font.pixelSize: 14; wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true; columns: 2; columnSpacing: 16; rowSpacing: 16
                            Repeater {
                                model: [
                                    { "title": "Controller", "body": "Windows process discovery, DPAPI-protected API configuration, asynchronous HTTPS and Material QML dashboard." },
                                    { "title": "Agent", "body": "Native in-process OpenGL renderer with safe JVM thread attachment, cached JNI bindings and reversible hooks." },
                                    { "title": "Privacy", "body": "The Hypixel key is encrypted for the current Windows account and is never exposed back to QML after saving." },
                                    { "title": "Scope", "body": "ESP marker rendering remains restricted to integrated single-player worlds; network statistics are separately match-gated." }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    Layout.fillWidth: true; Layout.preferredHeight: 150
                                    radius: 22; color: app.surfaceColor; border.width: 1; border.color: "#E7E0EC"
                                    ColumnLayout {
                                        anchors.fill: parent; anchors.margins: 20; spacing: 8
                                        Text { text: modelData.title; color: app.textColor; font.pixelSize: 17; font.weight: Font.DemiBold }
                                        Text { Layout.fillWidth: true; text: modelData.body; color: app.secondaryTextColor; font.pixelSize: 13; wrapMode: Text.WordWrap }
                                        Item { Layout.fillHeight: true }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Settings -------------------------------------------------------
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: settingsContent.implicitHeight + 68
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    ColumnLayout {
                        id: settingsContent
                        x: 34
                        y: 28
                        width: parent.width - 68
                        spacing: 20

                    Text {
                        text: "Injector settings"
                        color: app.textColor
                        font.pixelSize: 32
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: "Native JVM agent runtime and discovery preferences"
                        color: app.secondaryTextColor
                        font.pixelSize: 14
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 104
                        radius: 22
                        color: app.surfaceColor
                        border.width: 1
                        border.color: "#E7E0EC"

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: "Automatic process refresh"; color: app.textColor; font.pixelSize: 17; font.weight: Font.DemiBold }
                                Text { text: "Rescan Java processes every five seconds"; color: app.secondaryTextColor; font.pixelSize: 13 }
                            }
                            Switch {
                                checked: app.autoRefresh
                                onToggled: app.autoRefresh = checked
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 154
                        radius: 22
                        color: app.surfaceColor
                        border.width: 1
                        border.color: "#E7E0EC"

                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 20; spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Hypixel API key"; color: app.textColor; font.pixelSize: 17; font.weight: Font.DemiBold }
                                    Text { text: ApiKeys.statusMessage; color: ApiKeys.configured ? "#20853B" : app.secondaryTextColor; font.pixelSize: 12 }
                                }
                                MaterialButton {
                                    text: "Remove"; filled: false; visible: ApiKeys.configured
                                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                    foregroundColor: "#BA1A1A"; outlineColor: "#E7E0EC"
                                    onClicked: ApiKeys.clearKey()
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 10
                                MaterialTextField {
                                    id: apiKeyField
                                    Layout.fillWidth: true; Layout.preferredHeight: 52
                                    placeholderText: "Paste Hypixel developer API key"
                                    echoMode: TextInput.Password
                                    passwordCharacter: "●"
                                    selectByMouse: true
                                    maximumLength: 256
                                    Accessible.name: "Hypixel API key"
                                    onAccepted: if (ApiKeys.saveKey(text)) text = ""
                                }
                                MaterialButton {
                                    text: "Save securely"; filled: true; containerColor: app.primaryColor
                                    enabled: apiKeyField.text.trim().length >= 16
                                    onClicked: if (ApiKeys.saveKey(apiKeyField.text)) apiKeyField.text = ""
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 104
                        radius: 22
                        color: app.surfaceColor
                        border.width: 1
                        border.color: "#E7E0EC"

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 18
                            ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: "Click GUI hotkey"; color: app.textColor; font.pixelSize: 17; font.weight: Font.DemiBold }
                                Text { text: "Only this configurable key opens the in-game menu"; color: app.secondaryTextColor; font.pixelSize: 13 }
                            }
                            KeyCaptureButton {
                                Layout.preferredWidth: 190
                                virtualKey: OverlayManager.menuHotkey
                                primaryColor: app.primaryColor
                                surfaceColor: "#F7F2FA"
                                textColor: app.textColor
                                onKeyCaptured: function(key) { OverlayManager.menuHotkey = key }
                                Accessible.name: "Click GUI hotkey"
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 104
                        radius: 22
                        color: app.surfaceColor
                        border.width: 1
                        border.color: "#E7E0EC"

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 18
                            ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: "In-game interface size"; color: app.textColor; font.pixelSize: 17; font.weight: Font.DemiBold }
                                Text { text: "Four rasterized font sizes; synchronized with Click GUI"; color: app.secondaryTextColor; font.pixelSize: 13 }
                            }
                            Repeater {
                                model: ["S", "M", "L", "XL"]
                                delegate: MaterialButton {
                                    required property int index
                                    required property string modelData
                                    Layout.preferredWidth: 54
                                    Layout.preferredHeight: 44
                                    text: modelData
                                    filled: OverlayManager.guiScaleIndex === index
                                    containerColor: app.primaryColor
                                    foregroundColor: filled ? "white" : app.primaryColor
                                    outlineColor: "#CAC4D0"
                                    onClicked: OverlayManager.guiScaleIndex = index
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 176
                        radius: 22
                        color: "#EADDFF"

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 24
                            spacing: 20
                            Rectangle {
                                Layout.preferredWidth: 56
                                Layout.preferredHeight: 56
                                radius: 18
                                color: app.primaryColor
                                Text { anchors.centerIn: parent; text: "GPU"; color: "white"; font.pixelSize: 13; font.weight: Font.Bold }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 7
                                Text { text: "Dear ImGui + OpenGL 2"; color: app.primaryContainerText; font.pixelSize: 19; font.weight: Font.DemiBold }
                                Text {
                                    Layout.fillWidth: true
                                    text: "The controller first uses JVM Attach and can fall back to the standard Windows DLL loader when runtime Attach is unavailable. The agent renders before SwapBuffers in Minecraft's own LWJGL 2 OpenGL context."
                                    color: "#4F378B"
                                    font.pixelSize: 13
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 132
                        radius: 22
                        color: app.surfaceColor
                        border.width: 1
                        border.color: "#E7E0EC"

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 7
                            Text { text: "Minecraft 1.8.9 bindings"; color: app.textColor; font.pixelSize: 17; font.weight: Font.DemiBold }
                            Text {
                                Layout.fillWidth: true
                                text: "Forge release runtimes use SRG symbols; pure Vanilla uses embedded obfuscated 1.8.9 symbols. JNI reads health, entity IDs, positions, collision boxes and bed blocks without installing a target-side Mod or JAR."
                                color: app.secondaryTextColor
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
    }

    Rectangle {
        id: actionBar
        anchors.left: navigationRail.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 86
        color: "#FDF8FF"

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: "#DED8E2"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 34
            anchors.rightMargin: 26
            spacing: 14

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: app.activeTargetPid === 0
                          ? "No process selected"
                          : "Selected PID " + app.activeTargetPid
                    color: app.textColor
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    text: "Last scan: " + ProcessScanner.lastRefresh
                    color: app.secondaryTextColor
                    font.pixelSize: 11
                }
            }

            MaterialButton {
                readonly property bool selectedIsSession:
                    ProcessScanner.selectedPid !== 0
                    && ProcessScanner.selectedPid === OverlayManager.targetPid
                    && app.sessionAvailable
                text: OverlayManager.busy ? "Attaching…" : "Attach native overlay"
                iconText: "↗"
                containerColor: app.primaryColor
                // The process card already owns Return. Keep the bottom bar
                // for the one non-duplicate primary action only.
                visible: app.activeRoute === "scanner"
                         && ProcessScanner.selectedPid !== 0 && !selectedIsSession
                enabled: !OverlayManager.busy
                onClicked: app.openAttachDialog(ProcessScanner.selectedPid)
            }
        }
    }

    Timer {
        id: snackbarCloseTimer
        interval: 5600
        repeat: false
        onTriggered: injectionSnackbar.close()
    }

    // Material 3 snackbar: completion is announced only after the native
    // renderer reports RENDERER_READY (OverlayManager.Active), not merely when
    // the DLL handshake succeeds.
    Popup {
        id: injectionSnackbar
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: parent.height - height - 104
        width: Math.min(540, app.width - 56)
        height: 82
        padding: 0
        modal: false
        focus: false
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 18
            color: "#322F35"
            border.width: 1
            border.color: "#514D55"
        }

        contentItem: RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 10
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                radius: 13
                color: "#C9F8D1"
                Text {
                    anchors.centerIn: parent
                    text: "✓"
                    color: "#155724"
                    font.pixelSize: 19
                    font.weight: Font.Bold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: "Injection complete"
                    color: "#FFFBFE"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: "Native overlay is active · "
                          + app.menuHotkeyLabel(OverlayManager.menuHotkey)
                          + " opens Click GUI"
                    color: "#D0C8D1"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            MaterialButton {
                Layout.preferredWidth: 86
                Layout.preferredHeight: 40
                text: "Dismiss"
                filled: false
                foregroundColor: "#D0BCFF"
                outlineColor: "transparent"
                onClicked: {
                    snackbarCloseTimer.stop()
                    injectionSnackbar.close()
                }
            }
        }

        enter: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: 260
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                }
                NumberAnimation {
                    property: "scale"
                    from: 0.92
                    to: 1
                    duration: 340
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                }
            }
        }

        exit: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "opacity"
                    from: 1
                    to: 0
                    duration: 180
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
                }
                NumberAnimation {
                    property: "scale"
                    from: 1
                    to: 0.96
                    duration: 180
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
                }
            }
        }
    }

    Popup {
        id: attachDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, app.width - 80)
        height: 370
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0

        Overlay.modal: Rectangle {
            color: "#66000000"
            Behavior on opacity { NumberAnimation { duration: 180 } }
        }

        background: Rectangle {
            radius: 26
            color: app.surfaceColor
            border.width: 1
            border.color: "#E7E0EC"
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 16

            Rectangle {
                Layout.preferredWidth: 52
                Layout.preferredHeight: 52
                radius: 17
                color: app.primaryContainer
                Text { anchors.centerIn: parent; text: "↗"; color: app.primaryContainerText; font.pixelSize: 23; font.weight: Font.Bold }
            }
            Text {
                Layout.fillWidth: true
                text: "Load native in-game overlay?"
                color: app.textColor
                font.pixelSize: 24
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: (app.pendingProcess.windowTitle || "Selected Java process")
                      + "\nPID " + (app.pendingProcess.pid || "—")
                color: app.secondaryTextColor
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: "The controller first tries the official JVM Attach API. If runtime Attach is unavailable, a selected game JVM can use a visible LoadLibraryW fallback and the DLL's explicit startup export. No Mod JAR is copied into the game."
                color: app.secondaryTextColor
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                MaterialButton {
                    text: "Cancel"
                    filled: false
                    foregroundColor: app.primaryColor
                    outlineColor: "transparent"
                    onClicked: attachDialog.close()
                }
                MaterialButton {
                    text: OverlayManager.busy ? "Attaching…" : "Load DLL"
                    iconText: "↗"
                    containerColor: app.primaryColor
                    enabled: !OverlayManager.busy
                    onClicked: {
                        OverlayManager.interactive = false
                        if (OverlayManager.attachToProcess(app.pendingProcess.pid || 0)) {
                            attachDialog.close()
                            app.browsingProcesses = false
                            app.activeRoute = "main"
                        }
                    }
                }
            }
        }

        // Modal content uses the requested emphasized-decelerate expansion.
        enter: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: 300
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                }
                NumberAnimation {
                    property: "scale"
                    from: 0.86
                    to: 1
                    duration: 380
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.05, 0.7, 0.1, 1.0, 1.0, 1.0]
                }
            }
        }

        // Material standard-accelerate: cubic-bezier(0.3, 0, 0.8, 0.15).
        exit: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "opacity"
                    from: 1
                    to: 0
                    duration: 180
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
                }
                NumberAnimation {
                    property: "scale"
                    from: 1
                    to: 0.92
                    duration: 180
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: [0.3, 0.0, 0.8, 0.15, 1.0, 1.0]
                }
            }
        }
    }
}
