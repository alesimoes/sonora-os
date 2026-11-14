// SPDX-License-Identifier: GPL-3.0-or-later
//
// The selected note's readout and its four encoders.
// A fragment of PitchPanel.qml, spliced in at build time by qt/CMakeLists.txt
// where PitchPanel.qml says //#include "PitchInspector.qml"; it uses the ids pitchPanel,
// face and body from there, so it is not a standalone component.

    Rectangle {
        id: inspectorFace
        width: face.width - 2 * Theme.space4 - pitchPanel.canvasWidth - 58 - Theme.space3
        height: pitchPanel.canvasHeight + 2
        radius: Theme.radiusSmall
        color: Theme.raised
        border.color: Theme.borderStrong
        readonly property var ins: pitchPanel.inspector
        readonly property bool none: !ins || ins.none === true

        Column {
            anchors { fill: parent; margins: Theme.space3 }
            spacing: Theme.space3

            Rectangle {
                width: parent.width
                height: 74
                radius: Theme.radiusSmall
                color: "#0f1418"
                border.color: Theme.borderSubtle
                Text {
                    objectName: "pitchReadout"
                    anchors { left: parent.left; leftMargin: Theme.space3; top: parent.top; topMargin: Theme.space2 }
                    text: inspectorFace.none ? "--" : inspectorFace.ins.name
                    color: "#8fe3c0"
                    font { family: Theme.fontFamily; pixelSize: Theme.textDisplay; weight: Font.DemiBold; features: { "tnum": 1 } }
                }
                Text {
                    anchors { right: parent.right; rightMargin: Theme.space3; top: parent.top; topMargin: Theme.space2 + 4 }
                    text: inspectorFace.none ? "" : pitchPanel.signed(inspectorFace.ins.shift, 0) + " ct"
                    color: "#8fe3c0"
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; features: { "tnum": 1 } }
                }
                Text {
                    anchors { left: parent.left; leftMargin: Theme.space3; bottom: parent.bottom; bottomMargin: Theme.space2 }
                    text: inspectorFace.none ? "Select a note to edit it" : "found " + inspectorFace.ins.found
                    color: "#5d8f7a"
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }

            // Value, knob position, lit-arc origin; the signal per index below.
            Grid {
                columns: 2
                columnSpacing: Theme.space3
                rowSpacing: Theme.space2
                opacity: inspectorFace.none ? 0.4 : 1.0
                Repeater {
                    model: {
                        const i = inspectorFace.ins
                        const none = inspectorFace.none
                        return [
                            { label: "STRAIGHTEN", value: none ? "" : Math.round(i.straighten) + "%",
                              position: none ? 0 : i.straighten / 100, origin: 0 },
                            { label: "VIBRATO", value: none ? "" : Math.round(i.vibrato) + "%",
                              position: none ? 0.5 : i.vibrato / 200, origin: 0.5 },
                            { label: "FORMANT", value: none ? "" : pitchPanel.signed(i.formant, 0) + " ct",
                              position: none ? 0.5 : 0.5 + i.formant / 4800, origin: 0.5 },
                            { label: "GAIN", value: none ? "" : pitchPanel.signed(i.gain, 1) + " dB",
                              position: none ? 0.8 : (i.gain + 48) / 60, origin: 0.8 }
                        ]
                    }
                    Column {
                        id: encoder
                        required property int index
                        required property var modelData
                        width: 128
                        spacing: 2
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: encoder.modelData.label
                            color: Theme.textSecondary
                            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 }
                        }
                        Knob {
                            anchors.horizontalCenter: parent.horizontalCenter
                            diameter: 50
                            enabled: pitchPanel.canEdit && !inspectorFace.none
                            position: encoder.modelData.position
                            origin: encoder.modelData.origin
                            defaultPosition: encoder.modelData.origin
                            onMoved: (p) => {
                                if (encoder.index === 0) pitchPanel.straightenMoved(Math.round(p * 100))
                                else if (encoder.index === 1) pitchPanel.vibratoMoved(Math.round(p * 200))
                                else if (encoder.index === 2) pitchPanel.formantMoved(Math.round((p - 0.5) * 4800))
                                else pitchPanel.gainMoved(Math.round(p * 600 - 480) / 10)
                            }
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: encoder.modelData.value
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                        }
                    }
                }
            }
        }
    }
