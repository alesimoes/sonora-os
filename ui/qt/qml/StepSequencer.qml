// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Step Sequencer: a drum-machine faceplate. A grid of lanes by steps,
// click a cell to switch it on or off; the selected step's velocity,
// probability and pitch sit in a lane below it as three drag bars; swing and
// pattern length on the right of the header; mute and solo at each lane's
// head. A lane with nothing to trigger is drawn dimmed and says so: its
// cells can be set, but they will not sound.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property StepSequencerModel seq
    objectName: "stepSequencer"

    readonly property int cell: 30
    readonly property int headWidth: 132
    width: headWidth + 16 * (cell + 4) + 2 * Theme.space4
    height: 52 + seq.rows * (cell + 4) + 108
    // Up to 16 steps at full size; a longer pattern narrows its cells to fit.
    readonly property int cellW: Math.max(6, Math.min(cell, Math.floor(16 * (cell + 4) / seq.length) - 4))
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong

    component Bar: Item {
        id: bar
        property string label
        property int from: 0
        property int to: 100
        property int value: 0
        property string unit
        property bool enabled2: true
        signal picked(int v)
        width: 190; height: 36
        opacity: enabled2 ? 1 : 0.4
        Text {
            text: bar.label
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold }
        }
        Text {
            anchors.right: parent.right
            text: bar.enabled2 ? bar.value + bar.unit : "-"
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Rectangle {
            id: well
            y: 16; width: parent.width; height: 12
            radius: 3; color: Theme.sunken; border.color: Theme.borderSubtle
            Rectangle {
                readonly property real at: (bar.value - bar.from) / (bar.to - bar.from)
                width: Math.max(0, well.width * at); height: parent.height; radius: 3
                color: Theme.accent
                visible: bar.enabled2
            }
        }
        MouseArea {
            anchors.fill: well
            enabled: bar.enabled2
            function pick(x) {
                const f = Math.max(0, Math.min(1, x / width))
                bar.picked(Math.round(bar.from + f * (bar.to - bar.from)))
            }
            onPressed: mouse => pick(mouse.x)
            onPositionChanged: mouse => pick(mouse.x)
        }
    }

    component Stepper: Row {
        id: st
        property string label
        property int value
        property int step: 1
        property string unit
        signal nudged(int v)
        spacing: Theme.space2
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: st.label
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold }
        }
        SButton { text: "-"; width: 24; onClicked: st.nudged(st.value - st.step) }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: 40; horizontalAlignment: Text.AlignHCenter
            text: st.value + st.unit
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textBody }
        }
        SButton { text: "+"; width: 24; onClicked: st.nudged(st.value + st.step) }
    }

    // --- header: title, swing, length ---
    Item {
        id: header
        x: Theme.space4; y: Theme.space3
        width: win.width - 2 * Theme.space4; height: 32
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "STEP SEQUENCER"
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold; letterSpacing: 1.5 }
        }
        Row {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            spacing: Theme.space6
            Stepper {
                objectName: "swing"
                label: "SWING"; unit: "%"; step: 5
                value: seq.swing
                onNudged: v => seq.swing = v
            }
            Stepper {
                objectName: "length"
                label: "LENGTH"; unit: ""
                value: seq.length
                onNudged: v => seq.length = v
            }
        }
    }

    // --- the grid ---
    Column {
        id: grid
        x: Theme.space4; y: 52
        spacing: 4
        Repeater {
            model: seq.rows
            Row {
                id: lane
                required property int index
                readonly property bool sourced: (seq.revision, seq.rowHasSource(index))
                spacing: 4
                Item {
                    width: win.headWidth - 4; height: win.cell
                    opacity: lane.sourced ? 1 : 0.5
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: seq.rowName(lane.index) + (lane.sourced ? "" : "  (empty)")
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                    }
                    Row {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        spacing: 2
                        SButton {
                            objectName: "mute" + lane.index
                            text: "M"; width: 22; height: 22
                            active: (seq.revision, seq.rowMuted(lane.index))
                            activeColour: Theme.mute
                            onClicked: seq.toggleMute(lane.index)
                        }
                        SButton {
                            objectName: "solo" + lane.index
                            text: "S"; width: 22; height: 22
                            active: (seq.revision, seq.rowSoloed(lane.index))
                            activeColour: Theme.solo
                            onClicked: seq.toggleSolo(lane.index)
                        }
                    }
                }
                Repeater {
                    model: seq.length
                    Rectangle {
                        id: stepCell
                        required property int index
                        readonly property bool on: (seq.revision, seq.stepOn(lane.index, index))
                        readonly property bool picked: seq.selectedRow === lane.index
                                                       && seq.selectedStep === index
                        objectName: "cell" + lane.index + "_" + index
                        width: win.cellW; height: win.cell
                        radius: Theme.radiusSmall
                        color: on ? (lane.sourced ? Theme.accent : Theme.textTertiary)
                                  : (Math.floor(index / seq.stepsPerBeat) % 2 === 0 ? Theme.raised : Theme.sunken)
                        border.width: picked ? 2 : 1
                        border.color: picked ? Theme.text : Theme.borderSubtle
                        MouseArea {
                            anchors.fill: parent
                            onClicked: seq.toggleStep(lane.index, stepCell.index)
                        }
                    }
                }
            }
        }
    }

    // --- the selected step's lane ---
    Rectangle {
        id: stepLane
        objectName: "stepLane"
        x: Theme.space4; y: grid.y + grid.height + Theme.space3
        width: win.width - 2 * Theme.space4; height: 76
        radius: Theme.radius; color: Theme.window; border.color: Theme.borderSubtle
        readonly property bool live: seq.hasSelection
        Text {
            x: Theme.space3; y: Theme.space2
            text: stepLane.live
                  ? "STEP " + (seq.selectedStep + 1) + "  " + seq.rowName(seq.selectedRow)
                    + (seq.selectedOn ? "" : "  (off)")
                  : "SELECT A STEP"
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold }
        }
        Row {
            x: Theme.space3; y: 28
            spacing: Theme.space6
            Bar {
                objectName: "velocityBar"
                label: "VELOCITY"; to: 127
                value: seq.velocity; enabled2: stepLane.live
                onPicked: v => seq.velocity = v
            }
            Bar {
                objectName: "probabilityBar"
                label: "PROBABILITY"; unit: "%"
                value: seq.probability; enabled2: stepLane.live
                onPicked: v => seq.probability = v
            }
            Bar {
                objectName: "pitchBar"
                label: "PITCH"; from: -24; to: 24; unit: " st"
                value: seq.pitch; enabled2: stepLane.live
                onPicked: v => seq.pitch = v
            }
        }
    }
}
