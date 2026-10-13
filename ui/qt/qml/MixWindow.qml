// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window: one strip per track, in the Edit window's order, and the
// 2-channel monitor bus (session-format.md, "monitor_bus") at the right.
//
// The mix is session state and is kept and shown faithfully, but the engine
// has no playback path yet, so it changes nothing anyone hears. The line
// above the strips says so: a fader that looked live and was not would be a
// control that lies.
import QtQuick
import Sonora

Rectangle {
    id: mix
    required property ShellModel shell
    required property SessionModel session
    color: Theme.window

    Text {
        id: note
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space3; leftMargin: Theme.space4 }
        elide: Text.ElideRight
        text: "Mix state is kept with the session. The engine has no playback path yet: faders, pan, mute and solo change nothing you hear, and the bus meters have no signal."
        color: Theme.caution
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
    }

    Item {
        id: strips
        anchors {
            left: parent.left; right: bus.left; top: note.bottom; bottom: parent.bottom
            leftMargin: Theme.space3; rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3
        }
        readonly property real stripWidth: Math.floor(width / Math.max(1, mix.session.trackCount))

        Repeater {
            model: mix.session.trackNames
            ChannelStrip {
                required property int index
                required property string modelData
                x: index * strips.stripWidth
                width: strips.stripWidth - 2
                height: strips.height
                shell: mix.shell
                session: mix.session
                track: index
                name: modelData
                gainDb: mix.session.gains[index] || 0
                pan: mix.session.pans[index] || 0
                selected: mix.session.selectedTrack === index
            }
        }
    }

    ChannelStrip {
        id: bus
        anchors { right: parent.right; top: note.bottom; bottom: parent.bottom
                  rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3 }
        width: 92
        shell: mix.shell
        session: mix.session
        track: -1
        name: "Monitor"
        hasPan: false
        gainDb: mix.session.busGain
    }
}
