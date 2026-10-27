// SPDX-License-Identifier: GPL-3.0-or-later
//
// An aux bus's return strip (ADR-0022 §1): the bus's input, its balance,
// its mute, its own sends, a fader and the name, laid out as ChannelStrip
// so every fader starts at the same height. It returns to the monitor bus;
// its inserts, output, solo and meter come with ui.mixer.io, and the
// insert area is left empty until then.
//
// Fader: drag (Ctrl for fine), Alt-click for 0 dB, click the readout to
// type a gain. Pan: drag, Alt-click centres. The x by the name removes the
// bus and the sends that feed it, one undo step.
import QtQuick
import Sonora

Rectangle {
    id: aux
    required property ShellModel shell
    required property SessionModel session
    required property SendModel sends
    property int bus: 0
    property var info: ({ name: "", gainDb: 0, gainText: "0.0", pan: 0, panText: "C", mute: false })
    readonly property int strip: sends.trackCount + bus
    readonly property var stripInfo: strip < sends.strips.length ? sends.strips[strip] : null

    objectName: "aux" + bus
    color: Theme.panel
    radius: Theme.radius
    border.color: Theme.borderSubtle

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
        height: 3
        radius: Theme.radiusSmall
        color: Theme.accent
    }

    Column {
        id: top
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.space2 }
        spacing: Theme.space2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 2 * Theme.space1
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: aux.info.name
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }

        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 2 * Theme.space2
            height: 30
            Rectangle {
                id: panTrack
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 8; radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                Rectangle {
                    readonly property real c: panTrack.width / 2
                    x: aux.info.pan < 0 ? c + aux.info.pan * c : c
                    width: Math.max(2, Math.abs(aux.info.pan) * c)
                    height: parent.height
                    radius: Theme.radiusSmall
                    color: Theme.accent
                }
                Rectangle { x: panTrack.width / 2; width: 1; height: parent.height; color: Theme.textTertiary }
            }
            Text {
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
                text: aux.info.panText
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
            }
            MouseArea {
                objectName: "auxPan" + aux.bus
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 14
                property real lastX: 0
                onPressed: (m) => {
                    lastX = m.x
                    if (m.modifiers & Qt.AltModifier) aux.sends.auxPanDrag(aux.bus, -aux.info.pan * width / 2, width, false)
                }
                onPositionChanged: (m) => {
                    if (!pressed || (m.modifiers & Qt.AltModifier)) return
                    aux.sends.auxPanDrag(aux.bus, m.x - lastX, width, (m.modifiers & Qt.ControlModifier) !== 0)
                    lastX = m.x
                }
            }
        }

        SButton {
            objectName: "auxMute" + aux.bus
            anchors.horizontalCenter: parent.horizontalCenter
            text: "M"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
            active: aux.info.mute; activeColour: Theme.mute
            onClicked: aux.sends.toggleAuxMute(aux.bus)
        }
    }

    // No inserts on a return yet (ui.mixer.io): the space keeps the faders level.
    Item {
        id: insertArea
        anchors { left: parent.left; right: parent.right; top: top.bottom; topMargin: Theme.space2 }
        height: 10 * Theme.insertSlotHeight + 9 + 2 + 12
    }

    Item {
        id: sendArea
        anchors { left: parent.left; right: parent.right; top: insertArea.bottom; topMargin: Theme.space2
                  leftMargin: Theme.space1; rightMargin: Theme.space1 }
        height: 5 * Theme.insertSlotHeight + 4
        Column {
            width: parent.width
            spacing: 1
            Repeater {
                model: 5
                SendSlot {
                    required property int index
                    width: sendArea.width
                    sends: aux.sends
                    stripIndex: aux.strip
                    slot: aux.sends.bank + index
                    info: aux.stripInfo !== null ? aux.stripInfo.slots[aux.sends.bank + index]
                                                 : ({ present: false, letter: String.fromCharCode(65 + aux.sends.bank + index) })
                }
            }
        }
    }

    Item {
        id: faderArea
        anchors { left: parent.left; right: parent.right; top: sendArea.bottom; bottom: readout.top
                  topMargin: Theme.space3; bottomMargin: Theme.space2 }
        readonly property real capH: 26
        readonly property real travel: height - capH
        Rectangle {
            id: slotWell
            x: parent.width * 0.5 - width / 2
            y: faderArea.capH / 2
            width: 4
            height: faderArea.travel
            radius: 2
            color: Theme.sunken
            border.color: Theme.borderSubtle
        }
        Rectangle {
            width: Math.min(28, aux.width * 0.36)
            height: faderArea.capH
            x: slotWell.x + slotWell.width / 2 - width / 2
            y: (1 - aux.session.faderPosition(aux.info.gainDb)) * faderArea.travel
            radius: Theme.radius
            color: Theme.hover
            border.color: Theme.borderStrong
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 } height: 1; color: Theme.highlight }
            Rectangle { anchors.centerIn: parent; width: parent.width - 6; height: 2; color: Theme.text }
        }
        MouseArea {
            objectName: "auxFader" + aux.bus
            x: slotWell.x - 18; width: 36 + slotWell.width
            height: parent.height
            property real lastY: 0
            onPressed: (m) => {
                lastY = m.y
                if (m.modifiers & Qt.AltModifier) aux.sends.resetAuxGain(aux.bus)
            }
            onPositionChanged: (m) => {
                if (!pressed || (m.modifiers & Qt.AltModifier)) return
                aux.sends.auxFaderDrag(aux.bus, m.y - lastY, faderArea.travel,
                                       (m.modifiers & Qt.ControlModifier) !== 0)
                lastY = m.y
            }
        }
    }

    Rectangle {
        id: readout
        anchors { horizontalCenter: parent.horizontalCenter; bottom: nameplate.top; bottomMargin: Theme.space2 }
        width: parent.width - 2 * Theme.space1
        height: 18
        radius: Theme.radiusSmall
        color: Theme.sunken
        ValueField {
            id: gainField
            objectName: "auxGainValue" + aux.bus
            anchors.fill: parent
            shell: aux.shell
            text: aux.info.gainText
            textColour: aux.info.gainDb > 0.05 ? Theme.caution : Theme.text
            onSubmitted: (t) => { if (aux.sends.setAuxGainText(aux.bus, t)) gainField.finish() }
        }
    }
    Rectangle {
        id: nameplate
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.space1 }
        height: 22
        radius: Theme.radiusSmall
        color: Theme.raised
        Text {
            anchors { left: parent.left; right: remove.left; top: parent.top; bottom: parent.bottom; margins: 2 }
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: aux.info.name
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
        SButton {
            id: remove
            objectName: "auxRemove" + aux.bus
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 2 }
            text: "×"; implicitWidth: 16; implicitHeight: 16; fontPx: Theme.textCaption; bold: false
            onClicked: aux.sends.removeBus(aux.bus)
        }
    }
}
