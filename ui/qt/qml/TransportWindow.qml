// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window: Pro Tools' expanded Transport, floating over the
// Edit or Mix window. Buttons send exactly what their keys send, through the
// router; the counter is in the main counter's format (a click on the
// digits takes a position typed, Enter locates; beside them it cycles the
// format, as the bar's does), and beside it the position in every format at once, the way Logic's LCD shows bars and timecode
// together. The tempo and meter are the map's at that position.
//
// Pre/post-roll take a length typed in seconds, and the line under them says
// what they apply to: a punch, a loop take, a played selection, never a
// plain take. The punch range is set from the Edit selection or cleared, as
// the bar's PUNCH chip does. Click and Count-off send their keys' commands
// and light as the engine says (ADR-0020).
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    required property TransportModel transport
    // What a drag on the title moves: the item holding the window.
    property Item mover: win

    width: 620
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: transport.open

    // Taken by the window: a click on it never reaches the timeline beneath.
    MouseArea { anchors.fill: parent }

    // A soft shadow, the window's only depth.
    Rectangle {
        anchors { fill: parent; topMargin: 4; leftMargin: 4; rightMargin: -4; bottomMargin: -4 }
        radius: Theme.radius
        color: Theme.shadow
        z: -1
    }

    component Caption: Text {
        color: Theme.textTertiary
        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; capitalization: Font.AllUppercase; letterSpacing: 0.6 }
    }
    component Value: Text {
        color: Theme.text
        font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
    }
    // An inline component cannot see this file's ids, so it is handed the shell.
    component TransportKey: SButton {
        required property ShellModel shell
        property string command
        implicitWidth: 40; implicitHeight: 30; fontPx: Theme.textBody
        available: shell.implemented(command)
        onClicked: shell.invoke(command, -1)
    }

    Column {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space3 }
        spacing: Theme.space2

        // --- title: drag to move ---
        Item {
            width: parent.width
            height: 22
            MouseArea {
                anchors.fill: parent
                property point grab
                onPressed: (m) => grab = Qt.point(m.x, m.y)
                onPositionChanged: (m) => {
                    const room = win.mover.parent
                    win.mover.x = Math.max(0, Math.min(room.width - win.width, win.mover.x + m.x - grab.x))
                    win.mover.y = Math.max(0, Math.min(room.height - win.height, win.mover.y + m.y - grab.y))
                }
            }
            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text: "Transport"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                objectName: "transportMisses"
                visible: win.transport.playbackMisses > 0
                anchors { right: transportState.left; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                text: "Disk misses " + win.transport.playbackMisses
                color: Theme.alarm
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
            }
            Rectangle {
                id: transportState
                objectName: "transportState"
                anchors { right: closeButton.left; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                width: 84; height: 20; radius: Theme.radiusSmall
                color: win.transport.recording ? Theme.record
                     : win.transport.alive ? Theme.raised : Theme.alarmSurface
                Text {
                    anchors.centerIn: parent
                    text: win.transport.stateText
                    color: win.transport.recording ? Theme.textOnAccent
                         : win.transport.alive ? Theme.text : Theme.alarm
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.Bold }
                }
            }
            SButton {
                id: closeButton
                objectName: "transportClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                // What the key sends, so the key and the button never disagree.
                onClicked: win.shell.invoke("window.transport", -1)
            }
        }

        // --- the transport ---
        Row {
            spacing: Theme.space1
            TransportKey { shell: win.shell; text: "|◀"; command: "transport.return_to_zero" }
            TransportKey { shell: win.shell; text: "◀◀"; command: "transport.rewind" }
            TransportKey { shell: win.shell; text: "■"; command: "transport.stop"; fontPx: Theme.textTitle }
            TransportKey {
                shell: win.shell
                text: "▶"; command: "transport.toggle_play"; fontPx: Theme.textTitle
                active: win.transport.rolling && !win.transport.recording
                activeColour: Theme.playhead
            }
            TransportKey { shell: win.shell; text: "▶▶"; command: "transport.fast_forward" }
            TransportKey { shell: win.shell; text: "▶|"; command: "transport.go_to_end" }
            RecordButton {
                objectName: "transportRecord"
                shell: win.shell
                implicitWidth: 40; implicitHeight: 30; fontPx: Theme.textTitle
                ready: win.transport.recordReady
                recording: win.transport.recording
            }
            // Pro Tools: Loop Playback and Loop Record, two modes. Logic:
            // Cycle, one switch for both, so one button.
            TransportKey {
                objectName: "transportLoopPlayback"
                shell: win.shell
                text: win.shell.cycleMode ? "Cycle" : "Loop"
                command: win.shell.cycleMode ? "transport.toggle_cycle" : "transport.toggle_loop"
                implicitWidth: 48; fontPx: Theme.textSmall
                active: win.session.loopOn
            }
            TransportKey {
                objectName: "transportLoopRecord"
                visible: !win.shell.cycleMode
                shell: win.shell
                text: "Loop Rec"; command: "transport.toggle_loop_record"
                implicitWidth: 64; fontPx: Theme.textSmall
                active: win.session.loopRecordOn
                activeColour: Theme.record
            }
        }

        // --- where: the main counter, and every format ---
        Row {
            spacing: Theme.space3
            Rectangle {
                width: 260; height: 70
                radius: Theme.radius
                color: Theme.sunken
                border.color: Theme.borderSubtle
                // Beside the digits, a click cycles the format; on them, it
                // opens the counter for a position, as the bar's does.
                MouseArea { anchors.fill: parent; onClicked: win.session.cycleMainCounter() }
                Column {
                    anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                    Caption {
                        objectName: "transportCounterCaption"
                        width: 240
                        elide: Text.ElideRight
                        text: transportCounter.refused ? transportCounter.reason
                            : transportCounter.editing ? win.session.counterEntryHint + " · Enter locates"
                            : win.session.mainCounterFormat + " ▾ · " + win.transport.positionSource
                        color: transportCounter.refused ? Theme.alarm
                             : transportCounter.editing ? Theme.textSecondary : Theme.textTertiary
                        font.capitalization: transportCounter.editing ? Font.MixedCase : Font.AllUppercase
                    }
                    CounterField {
                        id: transportCounter
                        objectName: "transportCounter"
                        width: 236
                        shell: win.shell
                        session: win.session
                        fontPx: Theme.textDisplay
                        // In the main counter's format, from the same reading
                        // as the column beside it: two clocks a tick apart
                        // would show two different times side by side.
                        text: {
                            const f = win.session.mainCounterFormat
                            return f === "Timecode" ? win.transport.timecode
                                 : f === "Min:Secs" ? win.transport.minSecs
                                 : f === "Samples" ? win.transport.samples
                                 : win.transport.barsBeats
                        }
                        textColour: win.transport.recording ? Theme.record : Theme.text
                    }
                }
            }
            Grid {
                anchors.verticalCenter: parent.verticalCenter
                columns: 2
                columnSpacing: Theme.space3
                rowSpacing: 2
                Caption { text: "Bars|Beats" }
                Value { objectName: "transportBarsBeats"; text: win.transport.barsBeats }
                Caption { text: "Timecode" }
                Value { objectName: "transportTimecode"; text: win.transport.timecode }
                Caption { text: "Min:Secs" }
                Value { objectName: "transportMinSecs"; text: win.transport.minSecs }
                Caption { text: "Samples" }
                Value { text: win.transport.samples }
            }
        }

        Rectangle { width: parent.width; height: 1; color: Theme.borderSubtle }

        // --- tempo, meter, click ---
        Row {
            spacing: Theme.space6
            Column {
                Caption { text: "Tempo" }
                Text {
                    objectName: "transportTempo"
                    text: win.transport.tempoText
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.Medium; features: { "tnum": 1 } }
                }
            }
            Column {
                Caption { text: "Meter" }
                Text {
                    objectName: "transportMeter"
                    text: win.transport.meterText
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.Medium; features: { "tnum": 1 } }
                }
            }
            Column {
                width: 190
                Caption { text: win.transport.tempoChanges > 0 ? "Next change" : "Tempo map" }
                Value {
                    width: parent.width
                    elide: Text.ElideRight
                    text: win.transport.tempoChanges === 0 ? "one tempo, one meter"
                        : win.transport.nextChangeText.length > 0 ? win.transport.nextChangeText
                        : "none after here"
                    color: Theme.textSecondary
                }
            }
            Row {
                anchors.bottom: parent.bottom
                spacing: Theme.space1
                SButton {
                    objectName: "transportClick"
                    text: "Click"; implicitHeight: 24
                    active: win.transport.clickOn
                    onClicked: win.shell.invoke("transport.toggle_click", -1)
                }
                SButton {
                    objectName: "transportCountOff"
                    text: win.transport.countingOff ? "Counting" : "Count-off"
                    implicitHeight: 24
                    active: win.transport.countOffOn
                    onClicked: win.shell.invoke("transport.toggle_count_off", -1)
                }
            }
        }

        // --- the tempo map, when there is more than one entry ---
        Flow {
            width: parent.width
            visible: win.transport.tempoChanges > 0
            spacing: Theme.space1
            Repeater {
                model: win.transport.tempoMap
                Rectangle {
                    required property var modelData
                    width: entry.implicitWidth + 2 * Theme.space2
                    height: 20
                    radius: Theme.radiusSmall
                    color: modelData.current ? Theme.accent : Theme.raised
                    border.color: Theme.borderSubtle
                    Text {
                        id: entry
                        anchors.centerIn: parent
                        text: modelData.at + "  " + modelData.bpm + "  " + modelData.meter
                        color: modelData.current ? Theme.textOnAccent : Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                    }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: Theme.borderSubtle }

        // --- rolls, loop, punch: as the session holds them ---
        Row {
            spacing: Theme.space4
            // Typed in seconds, as Pro Tools' pre- and post-roll fields take
            // a length; a refusal leaves the field open with the reason on
            // the status line.
            Column {
                Caption { text: "Pre-roll" }
                ValueField {
                    id: preRollField
                    objectName: "transportPreRoll"
                    width: 76; height: 20
                    shell: win.shell
                    fontPx: Theme.textBody
                    text: win.transport.preRollText
                    onSubmitted: (t) => { if (win.session.setPreRollText(t)) preRollField.finish() }
                }
            }
            Column {
                Caption { text: "Post-roll" }
                ValueField {
                    id: postRollField
                    objectName: "transportPostRoll"
                    width: 76; height: 20
                    shell: win.shell
                    fontPx: Theme.textBody
                    text: win.transport.postRollText
                    onSubmitted: (t) => { if (win.session.setPostRollText(t)) postRollField.finish() }
                }
            }
            Column {
                Caption { text: win.shell.cycleMode ? "Cycle" : "Loop" }
                Value {
                    objectName: "transportLoop"
                    text: win.transport.rangeText(win.transport.loopStart, win.transport.loopEnd,
                                                  win.session.mainCounterFormat)
                    // A range kept with both loop modes off is there, unused.
                    color: !win.transport.loopSet ? Theme.textTertiary
                         : win.session.loopOn || win.session.loopRecordOn ? Theme.text
                         : Theme.textSecondary
                }
            }
            Column {
                Caption { text: "Punch" }
                Value {
                    objectName: "transportPunch"
                    text: win.transport.rangeText(win.transport.punchStart, win.transport.punchEnd,
                                                  win.session.mainCounterFormat)
                    color: win.transport.punchSet ? Theme.record : Theme.textTertiary
                }
            }
            // Pro Tools' punch points, Logic's autopunch locators: from the
            // Edit selection, or cleared. What the bar's PUNCH chip sends.
            Row {
                anchors.bottom: parent.bottom
                spacing: Theme.space1
                TransportKey {
                    objectName: "transportPunchSet"
                    shell: win.shell
                    text: "Set"; command: "transport.punch_selection"
                    implicitWidth: 40; implicitHeight: 22; fontPx: Theme.textSmall
                }
                TransportKey {
                    objectName: "transportPunchClear"
                    shell: win.shell
                    text: "Clear"; command: "transport.clear_punch"
                    implicitWidth: 44; implicitHeight: 22; fontPx: Theme.textSmall
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: win.transport.rollsWhy
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
