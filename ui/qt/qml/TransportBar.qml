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

    Row {
        id: left
        anchors { left: parent.left; leftMargin: Theme.space4; verticalCenter: parent.verticalCenter }
        spacing: Theme.space6

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
            SButton {
                text: "●"; implicitWidth: 40; implicitHeight: 32; fontPx: Theme.textTitle
                available: bar.shell.implemented("transport.record")
                active: bar.shell.recording
                activeColour: Theme.record
                onClicked: bar.shell.invoke("transport.record", -1)
            }
        }

        // --- counters ---
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 320; height: 54
            radius: Theme.radius
            color: Theme.sunken
            border.color: Theme.borderSubtle

            Column {
                anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                Text {
                    text: bar.session.mainCounterFormat
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; capitalization: Font.AllUppercase; letterSpacing: 0.6 }
                }
                Text {
                    objectName: "mainCounter"
                    // A dead engine's playhead is not current; the counter
                    // then shows the insertion point, which is ours.
                    text: bar.session.mainCounter
                    color: bar.shell.recording ? Theme.record : Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textCounter; weight: Font.Medium; features: { "tnum": 1 } }
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
            MouseArea { anchors.fill: parent; onClicked: bar.session.cycleMainCounter() }
        }

        // --- loop, punch, tempo: state only ---
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space1
            Row {
                spacing: Theme.space1
                Repeater {
                    model: [["LOOP", bar.session.loopOn], ["PUNCH", bar.session.punchOn]]
                    Rectangle {
                        required property var modelData
                        width: 52; height: 18; radius: Theme.radiusSmall
                        color: modelData[1] ? Theme.accent : Theme.raised
                        border.color: Theme.borderSubtle
                        Text {
                            anchors.centerIn: parent
                            text: modelData[0]
                            color: modelData[1] ? Theme.textOnAccent : Theme.textSecondary
                            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold; letterSpacing: 0.6 }
                        }
                    }
                }
            }
            Text {
                text: bar.session.tempoText
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
