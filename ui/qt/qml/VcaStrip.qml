// SPDX-License-Identifier: GPL-3.0-or-later
//
// A VCA master (ADR-0042): a fader and a mute that scale the tracks it
// holds, with no audio of its own. Laid out as AuxStrip below its name so
// every fader starts at the same height; the tracks it holds wear its name
// on their strips. "Set" makes it hold the selected tracks, the x by the
// name deletes it (one undo step; the tracks keep their faders).
//
// Fader: drag (Ctrl for fine), Alt-click for 0 dB, click the readout to
// type a gain.
import QtQuick
import Sonora

Rectangle {
    id: vca
    required property ShellModel shell
    required property SessionModel session
    required property MixModel mixer
    property int vcaIndex: 0
    // Where the other strips' fader zone starts, so every fader begins level.
    property real faderTop: 0
    property var info: ({ name: "", gainDb: 0, gainText: "0.0", mute: false, count: 0 })

    objectName: "vca" + vcaIndex
    color: Theme.panel
    radius: Theme.radius
    border.color: Theme.borderSubtle

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
        height: 3
        radius: Theme.radiusSmall
        color: Theme.caution
    }

    Column {
        id: top
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.space2 }
        spacing: Theme.space2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "VCA"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        Text {
            objectName: "vcaCount" + vca.vcaIndex
            anchors.horizontalCenter: parent.horizontalCenter
            text: vca.info.count === 1 ? "1 track" : vca.info.count + " tracks"
            color: vca.info.count === 0 ? Theme.caution : Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        SButton {
            objectName: "vcaMute" + vca.vcaIndex
            anchors.horizontalCenter: parent.horizontalCenter
            text: "M"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
            active: vca.info.mute; activeColour: Theme.mute
            onClicked: vca.mixer.toggleVcaMute(vca.vcaIndex)
        }
        SButton {
            objectName: "vcaSet" + vca.vcaIndex
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Set"; implicitHeight: 18; fontPx: Theme.textCaption; bold: false
            onClicked: vca.mixer.assignVca(vca.vcaIndex)
        }
    }

    Item {
        id: faderArea
        objectName: "vcaFaderZone" + vca.vcaIndex
        anchors { left: parent.left; right: parent.right; top: parent.top; bottom: readout.top
                  topMargin: Math.max(vca.faderTop, top.height + Theme.space4); bottomMargin: Theme.space2 }
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
            width: Math.min(28, vca.width * 0.36)
            height: faderArea.capH
            x: slotWell.x + slotWell.width / 2 - width / 2
            y: (1 - vca.session.faderPosition(vca.info.gainDb)) * faderArea.travel
            radius: Theme.radius
            color: Theme.hover
            border.color: Theme.borderStrong
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 } height: 1; color: Theme.highlight }
            Rectangle { anchors.centerIn: parent; width: parent.width - 6; height: 2; color: Theme.text }
        }
        MouseArea {
            objectName: "vcaFader" + vca.vcaIndex
            x: slotWell.x - 18; width: 36 + slotWell.width
            height: parent.height
            property real lastY: 0
            onPressed: (m) => {
                lastY = m.y
                if (m.modifiers & Qt.AltModifier) vca.mixer.resetVca(vca.vcaIndex)
            }
            onPositionChanged: (m) => {
                if (!pressed || (m.modifiers & Qt.AltModifier)) return
                vca.mixer.vcaDrag(vca.vcaIndex, m.y - lastY, faderArea.travel,
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
            objectName: "vcaGainValue" + vca.vcaIndex
            anchors.fill: parent
            shell: vca.shell
            text: vca.info.gainText
            textColour: vca.info.gainDb > 0.05 ? Theme.caution : Theme.text
            onSubmitted: (t) => { if (vca.mixer.setVcaGainText(vca.vcaIndex, t)) gainField.finish() }
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
            text: vca.info.name
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
        SButton {
            id: remove
            objectName: "vcaRemove" + vca.vcaIndex
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 2 }
            text: "×"; implicitWidth: 16; implicitHeight: 16; fontPx: Theme.textCaption; bold: false
            onClicked: vca.mixer.removeVca(vca.vcaIndex)
        }
    }
}
