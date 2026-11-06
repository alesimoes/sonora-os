// SPDX-License-Identifier: GPL-3.0-or-later
//
// The master's loudness (ITU-R BS.1770, EBU R128): integrated, momentary
// and short-term LUFS, loudness range and true peak, against a delivery
// target. Every number is the engine's, as the state block publishes it
// (LoudnessModel). A dash is a meter that has measured nothing yet; with the
// engine gone the panel says NO DATA over the whole of it, because a number
// from before the silence would look like a mix.
import QtQuick
import Sonora

Rectangle {
    id: panel
    required property ShellModel shell
    required property LoudnessModel loudness
    width: 176
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderSubtle

    readonly property bool live: loudness.alive

    component Row2: Item {
        id: r
        property string label
        property string value
        property string unit: "LUFS"
        property color tone: Theme.text
        property string tag
        width: parent.width
        height: 22
        Text {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            text: r.label
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Text {
            objectName: r.tag
            anchors { right: unitText.left; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
            text: panel.live ? r.value : "—"
            color: panel.live ? r.tone : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold; features: { "tnum": 1 } }
        }
        Text {
            id: unitText
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            width: 32
            text: r.unit
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }

    Column {
        anchors { fill: parent; margins: Theme.space3 }
        spacing: Theme.space1

        Text {
            text: "LOUDNESS"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 1; weight: Font.DemiBold }
        }
        Text {
            objectName: "loudnessNoData"
            visible: !panel.live
            width: parent.width
            wrapMode: Text.Wrap
            text: "NO DATA: the engine is not responding."
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.Bold }
        }

        // The number the delivery spec is written in.
        Text {
            objectName: "loudnessIntegrated"
            text: panel.live ? panel.loudness.integratedText : "—"
            color: !panel.live ? Theme.textTertiary
                   : panel.loudness.verdict === 2 ? Theme.caution : Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textDisplay; weight: Font.DemiBold; features: { "tnum": 1 } }
        }
        Text {
            text: "INTEGRATED LUFS"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 1 }
        }
        Text {
            objectName: "loudnessDelta"
            visible: panel.live && panel.loudness.deltaText.length > 0
            text: panel.loudness.deltaText + (panel.loudness.verdict === 1 ? "  on target" : "  off target")
            color: panel.loudness.verdict === 1 ? Theme.accent : Theme.caution
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold; features: { "tnum": 1 } }
        }

        Item { width: 1; height: Theme.space1 }
        Row2 { label: "Momentary"; tag: "loudnessMomentary"; value: panel.loudness.momentaryText }
        Row2 { label: "Short-term"; tag: "loudnessShortTerm"; value: panel.loudness.shortTermText }
        Row2 { label: "Range"; tag: "loudnessRange"; unit: "LU"; value: panel.loudness.rangeText }
        Row2 {
            label: "True peak"; tag: "loudnessTruePeak"; unit: "dBTP"
            value: panel.loudness.truePeakText
            tone: panel.loudness.truePeakOver ? Theme.alarm : Theme.text
        }
        Row2 { label: "Max M"; tag: "loudnessMaxM"; value: panel.loudness.maxMomentaryText }
        Row2 { label: "Max S"; tag: "loudnessMaxS"; value: panel.loudness.maxShortTermText }

        Item { width: 1; height: Theme.space1 }
        Text {
            objectName: "loudnessTarget"
            width: parent.width
            elide: Text.ElideRight
            text: "Target: " + panel.loudness.targetText
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Text {
            objectName: "loudnessCeiling"
            visible: panel.loudness.ceilingText.length > 0
            text: panel.loudness.ceilingText + (panel.loudness.truePeakOver ? ": exceeded" : "")
            color: panel.loudness.truePeakOver ? Theme.alarm : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: panel.loudness.truePeakOver ? Font.Bold : Font.Normal }
        }
        Flow {
            width: parent.width
            spacing: 2
            Repeater {
                model: panel.loudness.presets
                SButton {
                    required property int index
                    required property var modelData
                    objectName: "loudnessPreset" + index
                    text: modelData.label
                    implicitHeight: 20; fontPx: Theme.textCaption; bold: false
                    active: panel.loudness.preset === index
                    onClicked: panel.loudness.preset = panel.loudness.preset === index ? -1 : index
                }
            }
        }

        Item { width: 1; height: Theme.space1 }
        SButton {
            objectName: "loudnessReset"
            text: "Reset"
            width: parent.width
            implicitHeight: 22
            bold: false
            // Dimmed, still clickable: the click says why it cannot.
            available: panel.loudness.canReset
            onClicked: panel.loudness.reset()
        }
        // Held until the next reset goes through, not a status line's 2.5 s.
        Text {
            objectName: "loudnessRefusal"
            visible: panel.loudness.refusal.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            text: panel.loudness.refusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
        }
        Text {
            objectName: "loudnessWarning"
            visible: panel.loudness.warning.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            text: panel.loudness.warning
            color: Theme.caution
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
