// SPDX-License-Identifier: GPL-3.0-or-later
//
// A strip's automation mode (ADR-0027): the button names the mode, lit in
// its colour when the strip writes (Touch and Latch amber, Write red), and
// a click opens the list under it. Off and Read are quiet. Trim is listed
// and dimmed, because the engine refuses it; choosing it says so on the
// status line rather than showing a mode that does nothing.
import QtQuick
import Sonora

Item {
    id: sel
    required property MixModel mixer
    required property int track
    required property string mode
    property bool open: false
    readonly property var modes: ["Off", "Read", "Touch", "Latch", "Write", "Trim"]
    readonly property bool writes: mode === "Touch" || mode === "Latch" || mode === "Write"
    implicitHeight: 16
    z: open ? 100 : 0

    SButton {
        anchors.fill: parent
        text: sel.mode
        fontPx: Theme.textCaption
        bold: sel.writes
        active: sel.writes
        activeColour: sel.mode === "Write" ? Theme.record : Theme.caution
        onClicked: sel.open = !sel.open
    }

    Rectangle {
        visible: sel.open
        y: parent.height + 1
        width: Math.max(parent.width, 58)
        x: (parent.width - width) / 2
        height: sel.modes.length * 18 + 4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        Column {
            anchors { fill: parent; margins: 2 }
            spacing: 0
            Repeater {
                model: sel.modes
                SButton {
                    required property string modelData
                    objectName: "autoMode" + sel.track + modelData
                    width: parent.width
                    height: 18
                    text: modelData
                    alignLeft: true
                    fontPx: Theme.textCaption
                    active: modelData === sel.mode
                    activeColour: modelData === "Write" ? Theme.record
                                  : modelData === "Touch" || modelData === "Latch" ? Theme.caution
                                  : Theme.accent
                    available: modelData !== "Trim"
                    onClicked: {
                        sel.open = false
                        sel.mixer.setAutomationMode(sel.track, modelData)
                    }
                }
            }
        }
    }
}
