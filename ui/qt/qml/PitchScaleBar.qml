// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pitch panel's snap, key, scale and amount strip, split out of
// ui/pitch/qt/PitchPanel.qml (which passes itself as `panel`).
import QtQuick
import Sonora

Item {
    id: scaleBar
    required property var panel
    visible: panel.available
    height: 50

    Row {
        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
        spacing: Theme.space4

        Column {
            spacing: 4
            Text { text: "DRAG SNAP"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Row {
                spacing: 2
                Repeater {
                    model: ["Off", "Semitone", "Scale"]
                    SButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        active: panel.snapMode === index
                        implicitWidth: 74
                        onClicked: panel.snapModeChosen(index)
                    }
                }
            }
        }
        Column {
            spacing: 4
            Text { text: "KEY"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Row {
                spacing: 2
                Repeater {
                    model: panel.keyNames
                    SButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        active: panel.key === index
                        implicitWidth: 30
                        onClicked: panel.keyChosen(index)
                    }
                }
            }
        }
        Column {
            spacing: 4
            Text { text: "SCALE"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Row {
                spacing: 2
                SButton {
                    text: "<"
                    implicitWidth: 24
                    onClicked: panel.scaleChosen((panel.scale + panel.scaleNames.length - 1) % Math.max(1, panel.scaleNames.length))
                }
                Rectangle {
                    width: 132
                    height: 24
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: Theme.borderStrong
                    Text {
                        anchors.centerIn: parent
                        text: panel.scaleNames.length > panel.scale ? panel.scaleNames[panel.scale] : ""
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                    }
                }
                SButton {
                    text: ">"
                    implicitWidth: 24
                    onClicked: panel.scaleChosen((panel.scale + 1) % Math.max(1, panel.scaleNames.length))
                }
            }
        }
        Column {
            spacing: 4
            Text { text: "AMOUNT  " + Math.round(panel.amount) + "%"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Item {
                id: amountBar
                width: 120
                height: 24
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Theme.sunken
                    border.color: Theme.borderSubtle
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.amount / 100 * parent.width
                    height: 4
                    radius: 2
                    color: Theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: panel.canEdit
                    function set(x) { panel.amountMoved(Math.round(100 * Math.max(0, Math.min(1, x / width)))) }
                    onPressed: (mouse) => set(mouse.x)
                    onPositionChanged: (mouse) => set(mouse.x)
                }
            }
        }
        Column {
            spacing: 4
            Text { text: " "; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            SButton {
                text: "Snap Notes"
                available: panel.canEdit
                implicitWidth: 92
                onClicked: panel.snapNotes()
            }
        }
    }
}
