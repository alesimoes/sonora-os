// SPDX-License-Identifier: GPL-3.0-or-later
//
// One channel strip, top to bottom as a console reads: input, pan, record /
// input monitor / solo / mute, inserts A to J and the chain's delay, five
// sends (A-E or F-J, SendModel::bank), fader beside its meter, gain
// readout, name. `track` -1 is the monitor bus, which has a
// fader and two meters and nothing else; its insert area is left empty so
// every fader starts at the same height.
//
// Fader: drag to ride it, Ctrl-drag for fine (a tenth of the travel),
// Alt-click to return to 0 dB; click the readout under it to type a gain.
// Pan: drag the bar sideways (Ctrl for fine), Alt-click centres, click the
// value to type one. Solo and mute: Alt-click sets every track the same;
// Ctrl-click solo turns solo safe on or off (mix_model.h). A solo-safe
// track's solo button is recessed and carries a padlock, as Pro Tools greys
// it; another track's solo never outlines its mute, because the engine
// does not silence it. The taper is fader_taper.h's, asked through
// SessionModel, so the cap and the scale marks use the same mapping the
// tests check.
//
// The meter beside a track's fader reads the track's input, which is what
// the engine publishes (state_block.h); it says so over it. The monitor
// bus's two read the monitor bus after its fader, left and right.
import QtQuick
import Sonora

Rectangle {
    id: strip
    required property ShellModel shell
    required property SessionModel session
    // Rides, typed values and modifier clicks; null where they are not
    // wired (the shell's routing test), and the strip then rides through
    // SessionModel alone.
    property MixModel mixer: null
    property int track: 0
    property string name
    property real gainDb: 0
    // What the cap and readout show: the ride's value while this fader is
    // being ridden, the document's otherwise.
    readonly property real shownGain: mixer !== null && mixer.rideTrack === track ? mixer.rideGain : gainDb
    property real pan: 0
    property bool hasPan: true
    property bool selected: false
    // Null where no insert slots are wired (the shell's routing test).
    property InsertModel inserts: null
    // Null where no send slots are wired.
    property SendModel sends: null
    property Item dragHost: null
    readonly property var insertInfo: inserts !== null && !isBus && track < inserts.strips.length
                                      ? inserts.strips[track] : null
    readonly property alias insertColumn: insertColumn

    readonly property bool isBus: track < 0
    readonly property bool armed: !isBus && shell.engineAlive && shell.armed[track] === true
    readonly property bool soloed: !isBus && session.solos[track] === true
    readonly property bool muted: !isBus && session.mutes[track] === true
    readonly property bool soloSafe: !isBus && session.soloSafes[track] === true
    readonly property bool inputMonitor: !isBus && session.inputMonitors[track] === true
    // Another track's solo silences this one; a console shows that, so does
    // this. SessionModel applies the engine's rule, solo safe included.
    readonly property bool implicitlyMuted: !isBus && session.implicitMutes[track] === true
    // R, I, S and M share the strip's width: eighteen tracks across a
    // laptop screen leave about 70 px a strip.
    readonly property int stateButton: Math.max(12, Math.min(18, Math.floor((width - 10) / 4)))

    // The fader zone takes the rest, and the strip's height budget starts
    // from its floor: the sends keep their rows, and the inserts get what is
    // left above the fader and scroll inside their frame when they need
    // more (ui-design.md, "Mixer strip anatomy"). A strip too short to hold
    // five insert slots keeps those five and the fader gives way, so slots
    // A to E are never out of reach. The Mix window's views (MixModel) say
    // which sections are there: a collapsed one gives its height to the
    // fader, and with an F-J bank expanded the floor is the lower one.
    readonly property bool viewIO: mixer === null || mixer.showIO
    readonly property bool viewInsertsAE: mixer === null || mixer.showInsertsAE
    readonly property bool viewInsertsFJ: mixer !== null && mixer.showInsertsFJ
    readonly property bool viewSends: mixer === null || mixer.showSends
    readonly property int insertCount: (viewInsertsAE ? 5 : 0) + (viewInsertsFJ ? 5 : 0)
    readonly property int firstInsert: viewInsertsAE ? 0 : 5
    readonly property bool expanded: viewInsertsFJ || (viewSends && sends !== null && sends.bank === 5)
    readonly property real faderFloor: Math.ceil(height * (expanded ? Theme.faderShareExpandedPercent
                                                                    : Theme.faderSharePercent) / 100)
    readonly property real insertsFull: inserts === null || insertCount === 0 ? 0
        : insertCount * Theme.insertSlotHeight + (insertCount - 1) + 2 + 12
    readonly property real sendsFull: sends === null || !viewSends ? 0 : 5 * Theme.insertSlotHeight + 4
    readonly property real minSection: insertCount === 0 ? 0
        : Math.min(insertCount, Theme.stripMinSlots) * Theme.insertSlotHeight + 2 + 12
    readonly property real outputRow: viewIO ? 16 + Theme.space2 : 0
    readonly property real room: height - (Theme.space2 + top.height) - 2 * Theme.space2
                                 - faderFloor - (Theme.space3 + Theme.space2)
                                 - outputRow - (18 + Theme.space2 + 22 + Theme.space1)
    readonly property real sendsHeight: sendsFull
    // Both banks collapsed with a plugin in one: the area is only the "+N" chip's row.
    readonly property real insertsHeight: inserts === null ? 0
        : insertCount === 0 ? (hiddenInserts > 0 ? 12 : 0)
        : Math.min(insertsFull, Math.max(minSection, room - sendsHeight))
    readonly property alias insertFrame: insertFrame
    // Plugins in a collapsed insert bank, said on the strip ("+N").
    readonly property int hiddenInserts: {
        if (insertInfo === null) return 0
        let n = 0
        for (let i = 0; i < 10; ++i) {
            const shown = i < 5 ? viewInsertsAE : viewInsertsFJ
            if (!shown && insertInfo.slots[i].present) ++n
        }
        return n
    }

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
        // The selector's list drops over the inserts below.
        z: autoMode.open ? 100 : 0
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
            ValueField {
                id: panField
                objectName: strip.isBus ? "" : "panValue" + strip.track
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
                width: Math.min(parent.width, 44)
                height: 16
                shell: strip.shell
                text: strip.session.panText(strip.pan)
                textColour: Theme.textSecondary
                fontPx: Theme.textCaption
                editable: strip.mixer !== null
                onSubmitted: (t) => { if (strip.mixer.setPanText(strip.track, t)) panField.finish() }
            }
            // The bar and the space around it; the value below is a field.
            MouseArea {
                objectName: strip.isBus ? "" : "panArea" + strip.track
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 14
                property real lastX: 0
                onPressed: (m) => {
                    lastX = m.x
                    if (m.modifiers & Qt.AltModifier) {
                        strip.session.resetPan(strip.track)
                    } else if (strip.mixer !== null) {
                        holding = true
                        strip.mixer.beginPan(strip.track)
                    }
                }
                property bool holding: false
                function finish() {
                    if (holding) strip.mixer.endPan(strip.track)
                    holding = false
                }
                onReleased: finish()
                onCanceled: finish()
                // A window closed mid-press delivers no onCanceled.
                Component.onDestruction: finish()
                onPositionChanged: (m) => {
                    if (!pressed || (m.modifiers & Qt.AltModifier)) return
                    if (strip.mixer !== null) {
                        strip.mixer.panDrag(strip.track, m.x - lastX, width,
                                            (m.modifiers & Qt.ControlModifier) !== 0)
                    } else {
                        strip.session.panDrag(strip.track, m.x - lastX, width)
                    }
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
                text: "R"; implicitWidth: strip.stateButton; implicitHeight: 18; fontPx: Theme.textCaption
                active: strip.armed; activeColour: Theme.record
                available: strip.shell.engineAlive
                onClicked: strip.shell.invoke("track.toggle_arm", strip.track)
            }
            SButton {
                objectName: strip.isBus ? "" : "input" + strip.track
                text: "I"; implicitWidth: strip.stateButton; implicitHeight: 18; fontPx: Theme.textCaption
                active: strip.inputMonitor; activeColour: Theme.input
                onClicked: strip.shell.invoke("track.toggle_input_monitor", strip.track)
            }
            SoloButton {
                objectName: "solo" + strip.track
                implicitWidth: strip.stateButton; implicitHeight: 18; fontPx: Theme.textCaption
                soloed: strip.soloed
                safe: strip.soloSafe
                padlockName: strip.isBus ? "" : "soloSafe" + strip.track
                onClicked: (m) => {
                    if (strip.mixer === null || !strip.mixer.soloClick(strip.track, m.modifiers))
                        strip.shell.invoke("track.toggle_solo", strip.track)
                }
            }
            SButton {
                objectName: "mute" + strip.track
                text: "M"; implicitWidth: strip.stateButton; implicitHeight: 18; fontPx: Theme.textCaption
                active: strip.muted; activeColour: Theme.mute
                // Outlined in the mute colour when another track's solo
                // is what silences it.
                border.color: !active && strip.implicitlyMuted ? Theme.mute : Theme.borderSubtle
                onClicked: (m) => {
                    if (strip.mixer === null || !strip.mixer.muteClick(strip.track, m.modifiers))
                        strip.shell.invoke("track.toggle_mute", strip.track)
                }
            }
        }
        Item { visible: strip.isBus; width: 1; height: 18 }
        // The bus keeps its fader level with the tracks'.
        Item { visible: strip.isBus; width: 1; height: 16 }
        // Off, Read, Touch, Latch, Write: the mode the lanes play or write.
        AutomationModeButton {
            id: autoMode
            objectName: strip.isBus ? "" : "autoMode" + strip.track
            visible: !strip.isBus && strip.mixer !== null
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 2 * Theme.space2
            mixer: strip.mixer
            track: strip.track
            mode: strip.isBus || strip.track >= strip.session.automationModes.length
                  ? "Read" : strip.session.automationModes[strip.track]
        }
    }

    // --- inserts A to J, and the chain's delay ---
    Item {
        id: insertArea
        objectName: strip.isBus ? "" : "inserts" + strip.track
        anchors { left: parent.left; right: parent.right; top: top.bottom; topMargin: Theme.space2
                  leftMargin: Theme.space1; rightMargin: Theme.space1 }
        // The same on the bus (no inserts), which keeps its fader level with the tracks'.
        height: strip.insertsHeight
        visible: strip.inserts !== null && (strip.insertCount > 0 || strip.hiddenInserts > 0)

        ScrollFrame {
            id: insertFrame
            contentHeight: insertColumn.height
            objectName: strip.isBus ? "" : "insertFrame" + strip.track
            anchors { left: parent.left; right: parent.right; top: parent.top; bottom: delay.top }
            Column {
                id: insertColumn
                width: parent.width
                spacing: 1
                visible: !strip.isBus
                Repeater {
                    model: strip.isBus || strip.inserts === null ? 0 : strip.insertCount
                    InsertSlot {
                        required property int index
                        width: insertColumn.width
                        inserts: strip.inserts
                        dragHost: strip.dragHost
                        track: strip.track
                        slot: strip.firstInsert + index
                        info: strip.insertInfo !== null ? strip.insertInfo.slots[slot]
                                                        : ({ present: false, letter: String.fromCharCode(65 + slot) })
                    }
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
        // A collapsed bank with a plugin in it says so.
        Text {
            objectName: strip.isBus ? "" : "insertsMore" + strip.track
            anchors { right: parent.right; bottom: parent.bottom }
            height: 12
            visible: !strip.isBus && strip.hiddenInserts > 0
            text: "+" + strip.hiddenInserts
            color: Theme.accent
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold
                   features: { "tnum": 1 } }
        }
    }

    // --- sends: five of A to J ---
    Item {
        id: sendArea
        objectName: strip.isBus ? "" : "sends" + strip.track
        anchors { left: parent.left; right: parent.right; top: insertArea.bottom; topMargin: strip.sends === null ? 0 : Theme.space2
                  leftMargin: Theme.space1; rightMargin: Theme.space1 }
        height: strip.sendsHeight
        visible: strip.sends !== null && strip.viewSends
        readonly property var info: strip.sends !== null && !strip.isBus && strip.track < strip.sends.strips.length
                                    ? strip.sends.strips[strip.track] : null

        ScrollFrame {
            anchors.fill: parent
            objectName: strip.isBus ? "" : "sendFrame" + strip.track
            contentHeight: 5 * Theme.insertSlotHeight + 4
            Column {
                width: parent.width
                spacing: 1
                visible: !strip.isBus && sendArea.info !== null && sendArea.info.sends
                Repeater {
                    model: strip.isBus || strip.sends === null ? 0 : 5
                    SendSlot {
                        required property int index
                        width: sendArea.width
                        sends: strip.sends
                        stripIndex: strip.track
                        slot: strip.sends.bank + index
                        info: sendArea.info !== null ? sendArea.info.slots[strip.sends.bank + index]
                                                     : ({ present: false, letter: String.fromCharCode(65 + strip.sends.bank + index) })
                    }
                }
            }
        }
    }

    // --- output (ADR-0024): where the fader feeds; the Master's is its device ---
    SButton {
        id: outputButton
        objectName: strip.isBus ? "masterOutput" : "output" + strip.track
        readonly property var info: strip.sends !== null && !strip.isBus && strip.track < strip.sends.strips.length
                                    ? strip.sends.strips[strip.track] : null
        anchors { left: parent.left; right: parent.right; top: sendArea.bottom
                  topMargin: strip.viewIO ? Theme.space2 : 0; leftMargin: Theme.space1; rightMargin: Theme.space1 }
        implicitHeight: strip.viewIO ? 16 : 0
        visible: strip.viewIO && strip.sends !== null && (strip.isBus || (info !== null && info.sends))
        bold: false
        fontPx: Theme.textCaption
        text: strip.isBus ? "Out" : info !== null ? info.output : ""
        available: !strip.isBus
        onClicked: strip.sends.openOutputChooser(strip.track)
    }

    // --- fader and meter ---
    Item {
        id: faderArea
        objectName: strip.isBus ? "busFaderZone" : "faderZone" + strip.track
        anchors {
            left: parent.left; right: parent.right
            top: outputButton.bottom; bottom: readout.top
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
            y: (1 - strip.session.faderPosition(strip.shownGain)) * faderArea.travel
            radius: Theme.radius
            color: Theme.hover
            border.color: Theme.borderStrong
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 } height: 1; color: Theme.highlight }
            Rectangle { anchors.centerIn: parent; width: parent.width - 6; height: 2; color: Theme.text }
        }
        MouseArea {
            objectName: strip.isBus ? "busFader" : "fader" + strip.track
            x: slot.x - 18; width: 36 + slot.width
            height: parent.height
            property real lastY: 0
            property bool riding: false
            onPressed: (m) => {
                lastY = m.y
                if (m.modifiers & Qt.AltModifier) {
                    strip.session.resetGain(strip.track)
                } else if (strip.mixer !== null) {
                    riding = true
                    strip.mixer.beginRide(strip.track)
                }
            }
            onPositionChanged: (m) => {
                if (!pressed || (m.modifiers & Qt.AltModifier)) return
                const fine = (m.modifiers & Qt.ControlModifier) !== 0
                if (riding) strip.mixer.ride(strip.track, m.y - lastY, faderArea.travel, fine)
                else strip.session.faderDrag(strip.track, m.y - lastY, faderArea.travel, fine)
                lastY = m.y
            }
            function finish() {
                if (riding) strip.mixer.endRide(strip.track)
                riding = false
            }
            onReleased: finish()
            onCanceled: finish()
            Component.onDestruction: finish()
        }

        Row {
            id: meters
            anchors { right: parent.right; rightMargin: Theme.space1 + 1 }
            y: faderArea.capH / 2 - 4
            height: faderArea.travel + 8
            spacing: 2
            Meter {
                objectName: strip.isBus ? "busMeterL" : "mixMeter" + strip.track
                source: strip.shell
                channel: strip.isBus ? -1 : strip.track
                busSide: strip.isBus ? 0 : -1
                width: strip.isBus ? 8 : 10
                height: parent.height
            }
            Loader {
                active: strip.isBus
                width: 8
                height: parent.height
                sourceComponent: Meter {
                    objectName: "busMeterR"; channel: -1; busSide: 1; source: strip.shell
                }
            }
        }
        // What the meter reads, over it: a track's input, or for the bus,
        // that there is nothing to read.
        Text {
            objectName: strip.isBus ? "busMeterCaption" : "meterCaption" + strip.track
            visible: strip.mixer !== null
            anchors { right: meters.right; bottom: meters.top }
            text: strip.mixer === null ? "" : strip.isBus ? strip.mixer.busMeterCaption
                                                           : strip.mixer.trackMeterCaption
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold
                   letterSpacing: 0.5 }
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
        ValueField {
            id: gainField
            objectName: strip.isBus ? "busGainValue" : "gainValue" + strip.track
            anchors.fill: parent
            shell: strip.shell
            text: strip.session.gainText(strip.shownGain)
            textColour: strip.shownGain > 0.05 ? Theme.caution : Theme.text
            editable: strip.mixer !== null
            onSubmitted: (t) => { if (strip.mixer.setGainText(strip.track, t)) gainField.finish() }
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
