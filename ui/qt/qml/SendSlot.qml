// SPDX-License-Identifier: GPL-3.0-or-later
//
// One send slot, A to J, on a channel strip (ADR-0022). Empty: a sunken
// well with the slot's letter; click it to choose a bus. In use: the bus's
// name and the send's level, raised; "P" marks a pre-fader send, and a
// muted send is drawn in the mute colour, as Pro Tools greys it. Click a
// used slot for its send window (SendModel, send_controller.h).
import QtQuick
import Sonora

Rectangle {
    id: s
    property SendModel sends: null
    property int stripIndex: 0
    property int slot: 0
    property var info: ({ present: false, letter: "?" })
    readonly property bool present: info.present === true

    objectName: "send_" + stripIndex + "_" + slot
    height: Theme.insertSlotHeight
    radius: Theme.radiusSmall
    color: !present ? Theme.sunken
           : info.mute ? Theme.panel
           : (area.containsMouse ? Theme.hover : Theme.raised)
    border.width: 1
    border.color: present && info.mute ? Theme.mute : Theme.borderSubtle

    Text {
        id: busName
        anchors { left: parent.left; right: level.left; top: parent.top; bottom: parent.bottom
                  leftMargin: Theme.space1 }
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: s.present ? Text.AlignLeft : Text.AlignHCenter
        elide: Text.ElideRight
        text: !s.present ? s.info.letter : (s.info.pre ? "P " : "") + s.info.bus
        color: !s.present ? Theme.textTertiary : s.info.mute ? Theme.textSecondary : Theme.text
        font { family: Theme.fontFamily; pixelSize: Theme.textCaption
               weight: s.present ? Font.DemiBold : Font.Normal }
    }
    Text {
        id: level
        anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
        visible: s.present && s.width >= 56
        width: visible ? implicitWidth : 0
        text: s.present ? s.info.levelText : ""
        color: Theme.textSecondary
        font { family: Theme.fontFamily; pixelSize: Theme.textOverline; features: { "tnum": 1 } }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: if (s.sends !== null) s.sends.slotClicked(s.stripIndex, s.slot)
    }
}
