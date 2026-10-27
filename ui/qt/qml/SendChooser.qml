// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bus chooser, opened from an empty send slot or the send window's bus
// button (ADR-0022). Every aux bus is listed; one this send cannot reach
// (itself, a feedback loop, a bus another slot of the strip feeds) is
// dimmed with the reason, never hidden. "New bus" makes the bus and the
// send in one undo step, as Logic makes the aux strip with the bus.
// Escape or a click outside closes.
import QtQuick
import Sonora

Item {
    id: chooser
    property SendModel sends: null

    Rectangle {
        anchors.fill: parent
        color: Theme.shadow
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: chooser.sends.closeChooser()
        }
    }

    Rectangle {
        id: panel
        objectName: "sendChooserPanel"
        anchors.centerIn: parent
        width: 300
        height: Math.min(parent.height - 2 * Theme.space4, list.implicitHeight + title.height + 3 * Theme.space3)
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        focus: chooser.visible
        Keys.onEscapePressed: chooser.sends.closeChooser()

        Text {
            id: title
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space3 }
            elide: Text.ElideRight
            text: chooser.sends !== null ? chooser.sends.chooserTitle : ""
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }

        Column {
            id: list
            anchors { left: parent.left; right: parent.right; top: title.bottom; margins: Theme.space3 }
            spacing: 2
            Repeater {
                model: chooser.sends !== null ? chooser.sends.chooserItems : []
                Rectangle {
                    required property var modelData
                    required property int index
                    readonly property bool allowed: modelData.refusal.length === 0
                    objectName: "sendChoice" + index
                    width: list.width
                    height: modelData.refusal.length === 0 ? 24 : 36
                    radius: Theme.radiusSmall
                    color: allowed && area.containsMouse ? Theme.hover : Theme.raised
                    opacity: allowed ? 1.0 : 0.55
                    Text {
                        x: Theme.space2; y: 4
                        text: modelData.name
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                    }
                    Text {
                        visible: !parent.allowed
                        x: Theme.space2; y: 20
                        width: parent.width - 2 * Theme.space2
                        elide: Text.ElideRight
                        text: modelData.refusal
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: chooser.sends.choose(modelData.bus)
                    }
                }
            }
            SButton {
                objectName: "sendChoiceNew"
                width: list.width
                implicitHeight: 24
                bold: false
                fontPx: Theme.textSmall
                text: "New bus"
                onClicked: chooser.sends.choose(-1)
            }
        }
    }
}
