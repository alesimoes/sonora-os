// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window switch, transport, counters, loop and punch state, engine health.
// The transport buttons send exactly what their keys send, through the same
// router; a command this build cannot run is drawn unavailable and says so
// when pressed, rather than doing nothing.
import QtQuick
import Sonora

Rectangle {
    id: bar
    required property ShellModel shell
    required property SessionModel session
    // Null where no Transport window is wired: then the button is not shown.
    property TransportModel transport: null
    // Open lists the sessions folder in a panel the window owns.
    signal openRequested()
    // The File menu (Save, Save As, Save Copy In, Open), drawn by the window.
    signal fileMenuRequested(real x, real y)
    property bool fileMenuOpen: false
    // The Edit menu, drawn by the window under the point given.
    signal editMenuRequested(real x, real y)
    property bool editMenuOpen: false
    // The Track menu, the same way.
    signal trackMenuRequested(real x, real y)
    property bool trackMenuOpen: false
    // The Setup menu, the same way.
    signal setupMenuRequested(real x, real y)
    property bool setupMenuOpen: false

    height: Theme.transportHeight
    color: Theme.panel

    // The soft edge under the bar, its only depth.
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.bottom }
        height: 3
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.shadow }
            GradientStop { position: 1; color: "transparent" }
        }
        z: 10
    }

    // Why Record was refused, held until the next Record or Stop; over the
    // window below the bar, as the status line is too brief for it.
    Rectangle {
        objectName: "recordRefusal"
        visible: bar.shell.recordRefusal.length > 0
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.bottom; topMargin: Theme.space2 }
        width: refusalText.implicitWidth + 2 * Theme.space3
        height: refusalText.implicitHeight + Theme.space2
        radius: Theme.radius
        color: Theme.alarmSurface
        border.color: Theme.alarm
        z: 20
        Text {
            id: refusalText
            objectName: "recordRefusalText"
            anchors.centerIn: parent
            text: bar.shell.recordRefusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
        }
    }

    Row {
        id: left
        anchors { left: parent.left; leftMargin: Theme.space4; verticalCenter: parent.verticalCenter }
        // space4, not space6: with the Setup menu the bar is full at 1280 px.
        spacing: Theme.space4

        // --- Edit | Mix ---
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            SButton {
                text: "Edit"; fontPx: Theme.textBody; implicitHeight: 28; implicitWidth: 56
                active: bar.session.window === "edit"
                onClicked: if (!active) bar.shell.invoke("window.toggle_mix_edit", -1)
            }
            SButton {
                text: "Mix"; fontPx: Theme.textBody; implicitHeight: 28; implicitWidth: 56
                active: bar.session.window === "mix"
                onClicked: if (!active) bar.shell.invoke("window.toggle_mix_edit", -1)
            }
            // What the profile's Window > Transport key sends.
            SButton {
                objectName: "transportButton"
                visible: bar.transport !== null
                text: "Transport"; fontPx: Theme.textBody; implicitHeight: 28
                active: bar.transport !== null && bar.transport.open
                onClicked: bar.shell.invoke("window.transport", -1)
            }
        }

        // --- File: Pro Tools' File menu; Save in it sends what Ctrl+S sends ---
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            SButton {
                id: fileMenuButton
                objectName: "fileMenuButton"
                text: "File ▾"; implicitHeight: 28; implicitWidth: 56
                active: bar.fileMenuOpen
                onClicked: {
                    const p = fileMenuButton.mapToItem(null, 0, fileMenuButton.height)
                    bar.fileMenuRequested(p.x, p.y)
                }
            }
            SButton {
                objectName: "openButton"
                text: "Open"; implicitHeight: 28; implicitWidth: 52
                onClicked: bar.openRequested()
            }
            // Pro Tools' Edit menu: undo and redo by name, and the
            // clipboard. The arrow tells it from the Edit window's button.
            SButton {
                id: editMenuButton
                objectName: "editMenuButton"
                text: "Edit \u25BE"; implicitHeight: 28; implicitWidth: 56
                active: bar.editMenuOpen
                onClicked: {
                    const p = editMenuButton.mapToItem(null, 0, editMenuButton.height)
                    bar.editMenuRequested(p.x, p.y)
                }
            }
            // Pro Tools' Track menu (ADR-0023): new, duplicate, delete and
            // move, on the selected track.
            SButton {
                id: trackMenuButton
                objectName: "trackMenuButton"
                text: "Track ▾"; implicitHeight: 28; implicitWidth: 60
                active: bar.trackMenuOpen
                onClicked: {
                    const p = trackMenuButton.mapToItem(null, 0, trackMenuButton.height)
                    bar.trackMenuRequested(p.x, p.y)
                }
            }
            // Pro Tools' Setup menu: what belongs to the machine, not the
            // session (Latency, ADR-0019).
            SButton {
                id: setupMenuButton
                objectName: "setupMenuButton"
                text: "Setup \u25BE"; implicitHeight: 28; implicitWidth: 60
                active: bar.setupMenuOpen
                onClicked: {
                    const p = setupMenuButton.mapToItem(null, 0, setupMenuButton.height)
                    bar.setupMenuRequested(p.x, p.y)
                }
            }
        }

        // --- transport ---
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space1
            SButton {
                text: "|◀"; implicitWidth: 40; implicitHeight: 32; fontPx: Theme.textBody
                available: bar.shell.implemented("transport.return_to_zero")
                onClicked: bar.shell.invoke("transport.return_to_zero", -1)
            }
            SButton {
                text: "■"; implicitWidth: 40; implicitHeight: 32; fontPx: Theme.textTitle
                available: bar.shell.implemented("transport.stop")
                onClicked: bar.shell.invoke("transport.stop", -1)
            }
            SButton {
                text: "▶"; implicitWidth: 40; implicitHeight: 32; fontPx: Theme.textTitle
                available: bar.shell.implemented("transport.toggle_play")
                onClicked: bar.shell.invoke("transport.toggle_play", -1)
            }
            RecordButton {
                objectName: "barRecord"
                shell: bar.shell
                implicitWidth: 40; implicitHeight: 32; fontPx: Theme.textTitle
                ready: bar.shell.recordReady
                recording: bar.shell.recording
            }
        }

        // --- counters ---
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 320; height: 54
            radius: Theme.radius
            color: Theme.sunken
            border.color: Theme.borderSubtle

            // The format menu's stand-in: a click anywhere but the digits
            // cycles the format, as the arrow beside Pro Tools' counter
            // picks one.
            MouseArea { anchors.fill: parent; onClicked: bar.session.cycleMainCounter() }
            Column {
                anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                Text {
                    objectName: "mainCounterCaption"
                    // Open, the caption says what the field takes, or why
                    // what was typed is not a position.
                    width: 220
                    elide: Text.ElideRight
                    text: mainCounter.refused ? mainCounter.reason
                        : mainCounter.editing ? bar.session.counterEntryHint
                        : bar.session.mainCounterFormat + " ▾"
                    color: mainCounter.refused ? Theme.alarm
                         : mainCounter.editing ? Theme.textSecondary : Theme.textTertiary
                    font {
                        family: Theme.fontFamily; pixelSize: Theme.textCaption; letterSpacing: 0.6
                        capitalization: mainCounter.editing ? Font.MixedCase : Font.AllUppercase
                    }
                }
                CounterField {
                    id: mainCounter
                    objectName: "mainCounter"
                    width: 190
                    shell: bar.shell
                    session: bar.session
                    // A dead engine's playhead is not current; the counter
                    // then shows the insertion point, which is ours.
                    text: bar.session.mainCounter
                    textColour: bar.shell.recording ? Theme.record : Theme.text
                }
            }
            Column {
                anchors { right: parent.right; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                Text {
                    anchors.right: parent.right
                    text: bar.session.subCounterFormat
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; capitalization: Font.AllUppercase; letterSpacing: 0.6 }
                }
                Text {
                    anchors.right: parent.right
                    text: bar.session.subCounter
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                }
            }
        }

        // --- loop, loop record, punch, tempo ---
        // Each chip sends what its key sends, through the router. Loop
        // Record is shown apart from Loop in every profile: it alone makes
        // Record a loop take (ADR-0016, update "loop record as a record
        // mode"), and a take must never be armed as one invisibly. Cycle
        // profiles still see it, since a session saved in another profile
        // can hold the two apart.
        // In the two old chips' width, so the bar still fits 1280 px: the
        // loop modes on one row, the punch under them.
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space1
            Row {
                spacing: Theme.space1
                SButton {
                    objectName: "barLoop"
                    text: bar.shell.cycleMode ? "CYCLE" : "LOOP"
                    implicitWidth: 44; implicitHeight: 16; fontPx: Theme.textCaption
                    readonly property string command:
                        bar.shell.cycleMode ? "transport.toggle_cycle" : "transport.toggle_loop"
                    available: bar.shell.implemented(command)
                    active: bar.session.loopOn
                    onClicked: bar.shell.invoke(command, -1)
                }
                SButton {
                    objectName: "barLoopRecord"
                    text: "LOOP REC"
                    implicitWidth: 62; implicitHeight: 16; fontPx: Theme.textCaption
                    available: bar.shell.implemented("transport.toggle_loop_record")
                    active: bar.session.loopRecordOn
                    activeColour: Theme.record
                    onClicked: bar.shell.invoke("transport.toggle_loop_record", -1)
                }
            }
            // Lit while the session has a punch range: Record then punches
            // over it. Unlit, a press makes the Edit selection the range;
            // lit, it clears it.
            SButton {
                objectName: "barPunch"
                text: "PUNCH"
                implicitWidth: 108; implicitHeight: 16; fontPx: Theme.textCaption
                readonly property string command:
                    bar.session.punchOn ? "transport.clear_punch" : "transport.punch_selection"
                available: bar.shell.implemented(command)
                active: bar.session.punchOn
                activeColour: Theme.record
                onClicked: bar.shell.invoke(command, -1)
            }
            Text {
                objectName: "barTempo"
                // The map's tempo where the counter is, not its first entry.
                text: bar.transport !== null
                      ? bar.transport.tempoText + " bpm  " + bar.transport.meterText
                      : bar.session.tempoText
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
            }
        }

        // --- engine ---
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 64; height: 24; radius: Theme.radius
                color: bar.shell.recording ? Theme.record
                     : bar.shell.engineAlive ? Theme.raised : Theme.alarmSurface
                Text {
                    anchors.centerIn: parent
                    text: bar.shell.transportText
                    color: bar.shell.recording ? Theme.textOnAccent
                         : bar.shell.engineAlive ? Theme.text : Theme.alarm
                    font { family: Theme.fontFamily; pixelSize: bar.shell.engineAlive ? Theme.textBody : Theme.textCaption; weight: Font.Bold }
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    text: "Take " + (bar.shell.engineAlive ? bar.shell.take : "--")
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                }
                Text {
                    text: "xruns " + (bar.shell.engineAlive ? bar.shell.xruns : "--")
                    color: bar.shell.xruns > 0 ? Theme.alarm : Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                }
                Text {
                    text: "disk overruns " + (bar.shell.engineAlive ? bar.shell.overruns : "--")
                    color: bar.shell.overruns > 0 ? Theme.alarm : Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                }
            }
        }
    }

    Column {
        id: rightInfo
        anchors { right: parent.right; rightMargin: Theme.space4; verticalCenter: parent.verticalCenter }
        width: Math.max(0, parent.width - left.width - left.x - 2 * Theme.space6)
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideLeft
            visible: bar.shell.healthText.length > 0
            text: bar.shell.healthText
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.Bold }
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideLeft
            text: "Keys: " + bar.shell.profileName + " · " + bar.shell.formatText
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideLeft
            text: bar.shell.renderPath + " · meters " + bar.shell.meterHz + " Hz"
            color: bar.shell.softwareRendering ? Theme.alarm : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
