// SPDX-License-Identifier: GPL-3.0-or-later
//
// A track's solo button, in the Edit window's track header and on the Mix
// window's strip. Solo safe (TrackMix::solo_safe) recesses it, as Pro Tools
// greys a solo-safe button, and puts a padlock in its corner: the track
// stays audible whatever else is soloed, and the button says so before the
// operator solos something and wonders why it is still there. A solo-safe
// track can still be soloed itself, and then lights with the padlock on it.
import QtQuick
import Sonora

SButton {
    id: b
    property bool soloed: false
    property bool safe: false
    // Where the padlock can be found by the tests ("" for none).
    property string padlockName: ""

    text: "S"
    active: soloed
    activeColour: Theme.solo
    recessed: safe
    // The S steps left so the padlock sits beside it, not over it.
    labelOffset: safe ? -Math.round(width * 0.17) : 0

    Item {
        objectName: b.padlockName
        visible: b.safe
        anchors { right: parent.right; verticalCenter: parent.verticalCenter
                  rightMargin: Math.round(b.width * 0.12); verticalCenterOffset: 1 }
        width: 5; height: 7
        // The label's colour, so the padlock reads on both the recessed
        // button and a lit one.
        readonly property color ink: b.soloed ? Theme.textOnAccent : Theme.textSecondary
        Rectangle {
            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top }
            width: 3; height: 5; radius: 1.5
            color: "transparent"
            border { width: 1; color: parent.ink }
        }
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 4; radius: 1
            color: parent.ink
        }
    }
}
