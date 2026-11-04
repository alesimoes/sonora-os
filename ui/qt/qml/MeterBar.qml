// SPDX-License-Identifier: GPL-3.0-or-later
//
// One meter of a plugin's own telemetry (ADR-0057): a horizontal well, a bar,
// the name and the number. Gain reduction grows from the right, the way a
// compressor's meter falls; a level grows from the left.
//
// A meter the plugin has not published, or that stopped, draws no bar and
// says "no data": a bar at zero would read as silence, and the operator
// would trust it.
import QtQuick
import Sonora

Item {
    id: bar
    property string label
    property string kind: "other"     // "gain_reduction", "input", "output", "other"
    property bool valid: false
    property real fraction: 0         // 0..1 along the meter's range
    property string valueText: ""
    implicitHeight: 18

    readonly property bool fromRight: kind === "gain_reduction"
    // Levels turn amber near the top of the range and red at it; the gain
    // reduction is information, not a warning, and keeps one colour.
    readonly property color fill: kind === "gain_reduction" ? Theme.caution
                                  : fraction > 0.97 ? Theme.alarm
                                  : fraction > 0.88 ? Theme.caution : Theme.input

    Text {
        id: name
        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
        width: 28
        text: bar.label
        color: Theme.textSecondary
        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
    }
    Rectangle {
        id: well
        anchors { left: name.right; right: readout.left; rightMargin: Theme.space1
                  verticalCenter: parent.verticalCenter }
        height: 10
        radius: 2
        color: Theme.meterWell
        border.color: Theme.borderSubtle
        Rectangle {
            visible: bar.valid
            height: parent.height - 2
            y: 1
            radius: 1
            width: Math.max(0, (parent.width - 2) * bar.fraction)
            x: bar.fromRight ? parent.width - 1 - width : 1
            color: bar.fill
        }
    }
    Text {
        id: readout
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: 64
        horizontalAlignment: Text.AlignRight
        text: bar.valid ? bar.valueText : "no data"
        color: bar.valid ? Theme.text : Theme.textTertiary
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
    }
}
