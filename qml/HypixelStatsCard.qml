import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import McOverlay 1.0

Rectangle {
    id: root

    property color primaryColor: "#6750A4"
    property color textColor: "#1D1B20"
    property color secondaryTextColor: "#49454F"
    property color surfaceColor: "#FFFBFE"
    property color outlineColor: "#CAC4D0"

    implicitHeight: 326
    radius: 24
    color: surfaceColor
    border.width: 1
    border.color: "#E7E0EC"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                radius: 15
                color: "#EADDFF"
                Text {
                    anchors.centerIn: parent
                    text: "H"
                    color: "#21005D"
                    font.pixelSize: 18
                    font.weight: Font.Bold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: "Hypixel Bed Wars statistics"
                    color: root.textColor
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: "Official API · player-name lookup · six-hour memory cache"
                    color: root.secondaryTextColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            BusyIndicator {
                running: HypixelApi.busy
                visible: running
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            MaterialTextField {
                id: playerNameField
                Layout.fillWidth: true
                Layout.preferredHeight: 46
                placeholderText: "Minecraft player name"
                selectByMouse: true
                textColor: root.textColor
                focusedColor: root.primaryColor
                enabled: !HypixelApi.busy
                onAccepted: if (text.length > 0) HypixelApi.lookupPlayer(text)

                maximumLength: 16
                Accessible.name: "Minecraft player name"
            }

            MaterialButton {
                Layout.preferredWidth: 112
                Layout.preferredHeight: 46
                text: HypixelApi.busy ? "Loading…" : "Query"
                enabled: !HypixelApi.busy && playerNameField.text.trim().length > 0
                filled: true
                containerColor: root.primaryColor
                onClicked: HypixelApi.lookupPlayer(playerNameField.text)
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: width >= 680 ? 4 : 2
            columnSpacing: 10
            rowSpacing: 10

            Repeater {
                model: [
                    { "label": "WINS", "value": HypixelApi.wins.toString() },
                    { "label": "LOSSES", "value": HypixelApi.losses.toString() },
                    { "label": "W/L", "value": HypixelApi.winRate.toFixed(2) },
                    { "label": "FKDR", "value": HypixelApi.fkdr.toFixed(2) },
                    { "label": "FINAL KILLS", "value": HypixelApi.finalKills.toString() },
                    { "label": "FINAL DEATHS", "value": HypixelApi.finalDeaths.toString() },
                    { "label": "BEDS BROKEN", "value": HypixelApi.bedsBroken.toString() },
                    { "label": "BEDS LOST", "value": HypixelApi.bedsLost.toString() }
                ]

                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    radius: 16
                    color: "#F4EEF7"

                    Column {
                        anchors.centerIn: parent
                        spacing: 1
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData.value
                            color: root.textColor
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData.label
                            color: root.secondaryTextColor
                            font.pixelSize: 8
                            font.weight: Font.Bold
                            font.letterSpacing: 0.6
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: HypixelApi.state === HypixelApi.Ready ? "#20853B"
                      : (HypixelApi.state === HypixelApi.Error ? "#BA1A1A" : root.primaryColor)
            }
            Text {
                Layout.fillWidth: true
                text: HypixelApi.errorMessage.length > 0
                      ? HypixelApi.errorMessage
                      : (!HypixelApi.apiConfigured
                         ? "Add an API key in Settings to enable official queries"
                         : HypixelApi.statusMessage)
                color: HypixelApi.errorMessage.length > 0 ? "#BA1A1A" : root.secondaryTextColor
                font.pixelSize: 10
                elide: Text.ElideRight
            }
            Text {
                visible: HypixelApi.rateRemaining >= 0
                text: "API " + HypixelApi.rateRemaining + "/" + HypixelApi.rateLimit
                color: root.secondaryTextColor
                font.pixelSize: 9
                font.weight: Font.Medium
            }
        }
    }
}
