// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key chooser (ADR-0058), opened from the slot's key button:
// "None", then every aux bus by name; the one in use is marked. A stale key
// (a bus that is gone) is shown in the caution colour as missing and stays
// until another is chosen: a key that silently played nothing would lie.
// A plugin without a side-chain input is not offered the chooser at all
// (InsertSlot shows no button); the list is empty for it too.
// A loop is refused by the model and the refusal is held in the window.
// Escape or a click outside closes.
import QtQuick
import Sonora

Item {
    id: chooser
    property InsertModel inserts: null
    property int track: 0
    property int slot: 0
    property var info: ({})
    // Where the panel opens, in this item's own coordinates.
    property real anchorX: 0
    property real anchorY: 0
    signal closed()

    readonly property var choices: inserts !== null ? inserts.keyChoices(track, slot) : []
    readonly property bool missing: info.keyMissing === true

    z: 100
    focus: true
    Keys.onEscapePressed: chooser.closed()

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: chooser.closed()
    }

    Rectangle {
        id: panel
        objectName: "keyChooserPanel"
        x: Math.max(Theme.space1, Math.min(chooser.anchorX, chooser.width - width - Theme.space1))
        y: Math.max(Theme.space1, Math.min(chooser.anchorY, chooser.height - height - Theme.space1))
        width: 180
        height: head.height + list.height + 3 * Theme.space2
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong

        Text {
            id: head
            objectName: "keyChooserTitle"
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space2 }
            elide: Text.ElideRight
            text: chooser.missing ? "Key: (missing)"
                  : "Key: " + (chooser.info.keyName !== undefined && chooser.info.keyName.length > 0
                               ? chooser.info.keyName : "None")
            color: chooser.missing ? Theme.caution : Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }

        Column {
            id: list
            anchors { left: parent.left; right: parent.right; top: head.bottom; margins: Theme.space2 }
            spacing: 2
            Repeater {
                model: chooser.choices
                Rectangle {
                    required property var modelData
                    required property int index
                    objectName: "keyChoice" + index
                    width: list.width
                    height: 22
                    radius: Theme.radiusSmall
                    color: modelData.current ? Theme.accent
                           : choiceArea.containsMouse ? Theme.hover : Theme.raised
                    Text {
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: modelData.name
                        color: modelData.current ? Theme.textOnAccent : Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                    }
                    MouseArea {
                        id: choiceArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            chooser.inserts.setKey(chooser.track, chooser.slot, modelData.bus)
                            chooser.closed()
                        }
                    }
                }
            }
        }
    }
}
