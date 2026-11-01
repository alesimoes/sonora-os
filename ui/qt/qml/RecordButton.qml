// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Record button. Solid in the record colour while a take runs. While the
// engine is record-ready (Pro Tools: Record armed the take, Play starts it)
// it blinks at 1 Hz, and the outline stays in the record colour in the off
// phase, so the state reads at any instant. It sends what the profile's
// Record key sends (ShellModel.recordCommand).
import QtQuick
import Sonora

SButton {
    id: r
    required property ShellModel shell
    property bool ready: false
    property bool recording: false
    // The lit half of the blink; the first frame of record-ready is lit.
    property bool blinkOn: true
    // Holds the lit half: a still frame for a screenshot, or reduced motion.
    property bool steady: false

    text: "●"
    available: shell.implemented(shell.recordCommand)
    active: recording || (ready && (blinkOn || steady))
    activeColour: Theme.record
    border.color: ready && !recording ? Theme.record : (active ? Qt.darker(activeColour, 1.15) : Theme.borderSubtle)
    border.width: ready && !recording ? 2 : 1
    onClicked: shell.invoke(shell.recordCommand, -1)

    onReadyChanged: blinkOn = true

    Timer {
        interval: 500
        repeat: true
        running: r.ready && !r.recording && !r.steady
        onTriggered: r.blinkOn = !r.blinkOn
    }
}
