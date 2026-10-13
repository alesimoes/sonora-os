// SPDX-License-Identifier: GPL-3.0-or-later
//
// One channel strip, top to bottom as a console reads: input, pan, record /
// solo / mute, inserts A to J and the chain's delay, fader beside its
// meter, gain readout, name. `track` -1 is the monitor bus, which has a
// fader and two meters and nothing else; its insert area is left empty so
// every fader starts at the same height.
//
// Fader: drag to ride it, Ctrl-drag for fine (a tenth of the travel),
// Alt-click to return to 0 dB. Pan: drag sideways, Alt-click centres.
// The taper is fader_taper.h's, asked through SessionModel, so the cap and
// the scale marks use the same mapping the tests check.
import QtQuick
import Sonora

Rectangle {
    id: strip
    required property ShellModel shell
    required property SessionModel session
    property int track: 0
    property string name
    property real gainDb: 0
    property real pan: 0
    property bool hasPan: true
    property bool selected: false
    // Null where no insert slots are wired (the shell's routing test).
    property InsertModel inserts: null
    property Item dragHost: null
    readonly property var insertInfo: inserts !== null && !isBus && track < inserts.strips.length
                                      ? inserts.strips[track] : null
    readonly property alias insertColumn: insertColumn

    readonly property bool isBus: track < 0
    readonly property bool armed: !isBus && shell.engineAlive && shell.armed[track] === true
    readonly property bool soloed: !isBus && session.solos[track] === true
    readonly property bool muted: !isBus && session.mutes[track] === true
    // Another track's solo silences this one; a console shows that, so does this.
    readonly property bool implicitlyMuted: !isBus && session.anySolo && !soloed

    color: selected ? Theme.raised : Theme.panel
    radius: Theme.radius
    border.color: selected ? Theme.borderStrong : Theme.borderSubtle

    // Colour cap: the track's region colour.
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
        height: 3
        radius: Theme.radiusSmall
        color: strip.isBus ? Theme.textSecondary : Theme.trackColour(strip.track)
    }

    Column {
        id: top
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.space2 }
        spacing: Theme.space2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: strip.isBus ? "Out 1-2" : "In " + (strip.track + 1)
            color: strip.selected ? Theme.textSecondary : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
        }

        // Pan: a bar filled from the centre toward the side it leans.
        Item {
            visible: strip.hasPan
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
                    x: strip.pan < 0 ? c + strip.pan * c : c
                    width: Math.max(2, Math.abs(strip.pan) * c)
                    height: parent.height
                    radius: Theme.radiusSmall
                    color: Theme.accent
                }
                Rectangle { x: panTrack.width / 2; width: 1; height: parent.height; color: Theme.textTertiary }
            }
            Text {
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
                text: strip.session.panText(strip.pan)
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
            }
            MouseArea {
                anchors.fill: parent
                property real lastX: 0
                onPressed: (m) => {
                    lastX = m.x
                    if (m.modifiers & Qt.AltModifier) strip.session.resetPan(strip.track)
                }
                onPositionChanged: (m) => {
                    if (!pressed || (m.modifiers & Qt.AltModifier)) return
                    strip.session.panDrag(strip.track, m.x - lastX, width)
                    lastX = m.x
                }
            }
        }
        Item { visible: !strip.hasPan; width: 1; height: 30 }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 2
            visible: !strip.isBus
            SButton {
                text: "R"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
                active: strip.armed; activeColour: Theme.record
                available: strip.shell.engineAlive
                onClicked: strip.shell.invoke("track.toggle_arm", strip.track)
            }
            SButton {
                text: "S"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
                active: strip.soloed; activeColour: Theme.solo
                onClicked: strip.shell.invoke("track.toggle_solo", strip.track)
            }
            SButton {
                text: "M"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
                active: strip.muted; activeColour: Theme.mute
                // Outlined in the mute colour when another track's solo
                // is what silences it.
                border.color: !active && strip.implicitlyMuted ? Theme.mute : Theme.borderSubtle
                onClicked: strip.shell.invoke("track.toggle_mute", strip.track)
            }
        }
        Item { visible: strip.isBus; width: 1; height: 18 }
    }

    // --- inserts A to J, and the chain's delay ---
    Item {
        id: insertArea
        objectName: strip.isBus ? "" : "inserts" + strip.track
        anchors { left: parent.left; right: parent.right; top: top.bottom; topMargin: Theme.space2
                  leftMargin: Theme.space1; rightMargin: Theme.space1 }
        // Fixed, so the bus (no inserts) keeps its fader level with the tracks'.
        height: strip.inserts === null ? 0 : 10 * Theme.insertSlotHeight + 9 + 2 + delay.height
        visible: strip.inserts !== null

        Column {
            id: insertColumn
            width: parent.width
            spacing: 1
            visible: !strip.isBus
            Repeater {
                model: strip.isBus || strip.inserts === null ? 0 : 10
                InsertSlot {
                    required property int index
                    width: insertColumn.width
                    inserts: strip.inserts
                    dragHost: strip.dragHost
                    track: strip.track
                    slot: index
                    info: strip.insertInfo !== null ? strip.insertInfo.slots[index]
                                                    : ({ present: false, letter: String.fromCharCode(65 + index) })
                }
            }
        }
        Text {
            id: delay
            objectName: strip.isBus ? "" : "delay" + strip.track
            anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
            height: 12
            visible: !strip.isBus
            text: strip.insertInfo !== null ? strip.insertInfo.latencyText : ""
            color: strip.insertInfo !== null && strip.insertInfo.latency > 0 ? Theme.textSecondary : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
        }
    }

    // --- fader and meter ---
    Item {
        id: faderArea
        anchors {
            left: parent.left; right: parent.right
            top: insertArea.bottom; bottom: readout.top
            topMargin: Theme.space3; bottomMargin: Theme.space2
        }
        readonly property real capH: 26
        readonly property real travel: height - capH

        // Scale marks at the taper's own positions.
        Repeater {
            model: strip.width >= 56 ? [12, 6, 0, -5, -10, -20, -30, -40, -60] : [12, 0, -10, -30, -60]
            Text {
                required property int modelData
                x: 2
                y: faderArea.capH / 2 + (1 - strip.session.faderPosition(modelData)) * faderArea.travel - height / 2
                text: modelData > 0 ? "+" + modelData : modelData
                color: strip.selected ? Theme.textSecondary : Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textOverline; features: { "tnum": 1 } }
            }
        }

        // The slot.
        Rectangle {
            id: slot
            x: parent.width * 0.5 - width / 2
            y: faderArea.capH / 2
            width: 4
            height: faderArea.travel
            radius: 2
            color: Theme.sunken
            border.color: Theme.borderSubtle
        }
        // The cap: a raised rectangle with a line across it at the value.
        Rectangle {
            id: cap
            objectName: "faderCap"
            width: Math.min(28, strip.width * 0.36)
            height: faderArea.capH
            x: slot.x + slot.width / 2 - width / 2
            y: (1 - strip.session.faderPosition(strip.gainDb)) * faderArea.travel
            radius: Theme.radius
            color: Theme.hover
            border.color: Theme.borderStrong
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 } height: 1; color: Theme.highlight }
            Rectangle { anchors.centerIn: parent; width: parent.width - 6; height: 2; color: Theme.text }
        }
        MouseArea {
            x: slot.x - 18; width: 36 + slot.width
            height: parent.height
            property real lastY: 0
            onPressed: (m) => {
                lastY = m.y
                if (m.modifiers & Qt.AltModifier) strip.session.resetGain(strip.track)
            }
            onPositionChanged: (m) => {
                if (!pressed || (m.modifiers & Qt.AltModifier)) return
                strip.session.faderDrag(strip.track, m.y - lastY, faderArea.travel,
                                        (m.modifiers & Qt.ControlModifier) !== 0)
                lastY = m.y
            }
        }

        Row {
            id: meters
            anchors { right: parent.right; rightMargin: Theme.space1 + 1 }
            y: faderArea.capH / 2 - 4
            height: faderArea.travel + 8
            spacing: 2
            Meter {
                objectName: strip.isBus ? "busMeterL" : "mixMeter" + strip.track
                source: strip.isBus ? null : strip.shell
                channel: strip.isBus ? -1 : strip.track
                width: strip.isBus ? 8 : 10
                height: parent.height
            }
            Loader {
                active: strip.isBus
                width: 8
                height: parent.height
                sourceComponent: Meter { objectName: "busMeterR"; channel: -1 }
            }
        }
    }

    // --- readout and name ---
    Rectangle {
        id: readout
        anchors { horizontalCenter: parent.horizontalCenter; bottom: nameplate.top; bottomMargin: Theme.space2 }
        width: parent.width - 2 * Theme.space1
        height: 18
        radius: Theme.radiusSmall
        color: Theme.sunken
        Text {
            anchors.centerIn: parent
            text: strip.session.gainText(strip.gainDb)
            color: strip.gainDb > 0.05 ? Theme.caution : Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
        }
    }
    Rectangle {
        id: nameplate
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.space1 }
        height: 22
        radius: Theme.radiusSmall
        color: strip.selected ? Theme.hover : Theme.raised
        Text {
            anchors.fill: parent
            anchors.margins: 2
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: strip.name
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
        MouseArea {
            anchors.fill: parent
            enabled: !strip.isBus
            onClicked: strip.session.selectTrack(strip.track)
        }
    }
}
