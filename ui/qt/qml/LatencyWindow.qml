// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Latency (ADR-0019, update of 2026-10-26): the round trip takes
// are moved back by, where it came from, and the loopback probe. Cable an
// output to an input, pick both, Measure; or type the figure. The engine
// keeps a measured or typed figure for this interface, rate and period.
// A refusal stays in the window until the next action.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    property Item mover: win
    property int output: 2
    property int input: Math.max(1, shell.trackCount)

    width: 440
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.latencyOpen

    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors { fill: parent; topMargin: 4; leftMargin: 4; rightMargin: -4; bottomMargin: -4 }
        radius: Theme.radius
        color: Theme.shadow
        z: -1
    }

    // A port number with − and + beside it, kept within 1..`highest`.
    component PortPicker: Row {
        id: picker
        property string label
        property string prefix
        property int value: 1
        property int highest: 1
        signal moved(int value)
        spacing: Theme.space1
        Text {
            width: 52
            anchors.verticalCenter: parent.verticalCenter
            text: picker.label
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        SButton {
            objectName: picker.objectName + "Down"
            text: "−"; implicitWidth: 24; implicitHeight: 22
            available: picker.value > 1
            onClicked: if (picker.value > 1) picker.moved(picker.value - 1)
        }
        Rectangle {
            width: 64; height: 22
            radius: Theme.radiusSmall
            color: Theme.sunken
            border.color: Theme.borderSubtle
            Text {
                objectName: picker.objectName + "Value"
                anchors.centerIn: parent
                text: picker.prefix + picker.value
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
            }
        }
        SButton {
            objectName: picker.objectName + "Up"
            text: "+"; implicitWidth: 24; implicitHeight: 22
            available: picker.value < picker.highest
            onClicked: if (picker.value < picker.highest) picker.moved(picker.value + 1)
        }
    }

    Column {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space3 }
        spacing: Theme.space2

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
                text: "Latency"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "latencyClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.shell.invoke("window.latency", -1)
            }
        }

        // --- the figure in effect ---
        Rectangle {
            width: parent.width
            height: figure.implicitHeight + 2 * Theme.space2
            radius: Theme.radiusSmall
            color: Theme.sunken
            border.color: Theme.borderSubtle
            Column {
                id: figure
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space2 }
                spacing: 2
                Text {
                    text: "ROUND TRIP"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
                }
                Row {
                    spacing: Theme.space2
                    Text {
                        id: framesText
                        objectName: "latencyFrames"
                        text: win.shell.latencyKnown ? win.shell.latencyFrames + " frames" : "—"
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textCounter; features: { "tnum": 1 } }
                    }
                    Text {
                        anchors.baseline: framesText.baseline
                        text: win.shell.latencyKnown ? win.shell.latencyMs : ""
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                    }
                }
                Text {
                    objectName: "latencySource"
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: win.shell.latencyKnown ? win.shell.latencySource
                                                 : "this engine says nothing of its round trip"
                    color: win.shell.latencyKnown && win.shell.latencySource.indexOf("measured") === 0
                           ? Theme.input : Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
        }

        // --- the probe ---
        Text {
            text: "MEASURE THROUGH A CABLE"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
        }
        PortPicker {
            objectName: "latencyOutput"
            label: "Output"
            prefix: "out_"
            value: win.output
            highest: Math.max(1, win.shell.latencyOutputs)
            onMoved: (v) => win.output = v
        }
        PortPicker {
            objectName: "latencyInput"
            label: "Input"
            prefix: "in_"
            value: win.input
            highest: Math.max(1, win.shell.trackCount)
            onMoved: (v) => win.input = v
        }
        Row {
            spacing: Theme.space2
            SButton {
                objectName: "latencyMeasure"
                text: "Measure"; implicitWidth: 96
                available: !win.shell.latencyMeasuring
                onClicked: win.shell.measureLatency(win.output, win.input)
            }
            SButton {
                objectName: "latencyCancel"
                text: "Cancel"; implicitWidth: 80
                available: win.shell.latencyMeasuring
                onClicked: win.shell.cancelLatency()
            }
        }
        Text {
            objectName: "latencyResult"
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: win.shell.latencyResult
            color: win.shell.latencyFailed ? Theme.caution
                 : win.shell.latencyMeasuring ? Theme.accent : Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }

        // --- by hand ---
        Text {
            text: "SET BY HAND"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
        }
        Row {
            spacing: Theme.space2
            Rectangle {
                width: 96; height: 24
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: manualInput.activeFocus ? Theme.accent : Theme.borderStrong
                TextInput {
                    id: manualInput
                    objectName: "latencyManualInput"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    horizontalAlignment: TextInput.AlignRight
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.text
                    validator: IntValidator { bottom: 0; top: 999999 }
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                    onActiveFocusChanged: win.shell.setTextEntry(activeFocus)
                    Keys.onEscapePressed: focus = false
                    Keys.onReturnPressed: setButton.clicked(null)
                    Keys.onEnterPressed: setButton.clicked(null)
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "frames"
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            SButton {
                id: setButton
                objectName: "latencySet"
                text: "Set"; implicitWidth: 64
                available: !win.shell.latencyMeasuring && manualInput.text.length > 0
                onClicked: {
                    if (manualInput.text.length === 0) return
                    win.shell.setLatency(parseInt(manualInput.text))
                    manualInput.focus = false
                }
            }
        }

        Text {
            objectName: "latencyRefusal"
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: win.shell.latencyRefusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Takes are placed this much earlier, so they line up with what you heard. Kept for this interface, rate and buffer size; not changed during a take."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
