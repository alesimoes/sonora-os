// SPDX-License-Identifier: GPL-3.0-or-later
//
// A strip's automation mode (ADR-0027): the button names the mode, lit in
// its colour when the strip writes (Touch and Latch amber, Write red), and
// a click opens the list under it. Off and Read are quiet. Trim is listed
// and dimmed, because the engine refuses it; choosing it says so on the
// status line rather than showing a mode that does nothing.
//
// The open list is a menu. A press anywhere off it, Escape, or any other
// key closes it, and whatever closed it does nothing else: a click-away
// that also soloed the track under it would be two actions for one. Up and
// Down move through the modes the engine plays, Return or Space chooses.
// While the list is open the keys are its own (ShellModel::setTextEntry),
// as a text field's are; a key it has no use for closes it and says so on
// the status line, rather than vanishing.
import QtQuick
import Sonora

Item {
    id: sel
    required property ShellModel shell
    required property MixModel mixer
    required property int track
    required property string mode
    property bool open: false
    readonly property var modes: ["Off", "Read", "Touch", "Latch", "Write", "Trim"]
    readonly property bool writes: mode === "Touch" || mode === "Latch" || mode === "Write"
    // The row the keyboard is on while the list is open.
    property int hot: -1
    implicitHeight: 16

    // The application's root item, the one under the window's own content
    // item, which fills the window.
    function topItem() {
        let p = sel
        while (p.parent !== null && p.parent.parent !== null) p = p.parent
        return p
    }
    function choose(m) {
        sel.open = false
        sel.mixer.setAutomationMode(sel.track, m)
    }
    function step(by) {
        let i = sel.hot
        do {
            i = (i + by + sel.modes.length) % sel.modes.length
        } while (sel.modes[i] === "Trim")
        sel.hot = i
    }

    onOpenChanged: {
        if (open) {
            // The list goes to the window's top layer, so nothing drawn
            // after this strip covers it and the catcher spans the window.
            const top = sel.topItem()
            layer.parent = top
            const p = sel.mapToItem(top, 0, 0)
            list.x = Math.max(0, Math.min(p.x + (sel.width - list.width) / 2, top.width - list.width))
            // Below the button, or above it where the window ends first
            // (an Edit track header near the bottom).
            const below = p.y + sel.height + 1
            list.y = below + list.height <= top.height ? below : Math.max(0, p.y - list.height - 1)
            sel.hot = sel.modes.indexOf(sel.mode)
            list.forceActiveFocus()
        } else {
            list.focus = false
            sel.shell.setTextEntry(false)
            layer.parent = sel
        }
    }
    // A strip removed with its list open must not leave the profile locked out.
    Component.onDestruction: if (sel.open) sel.shell.setTextEntry(false)

    SButton {
        anchors.fill: parent
        text: sel.mode
        fontPx: Theme.textCaption
        bold: sel.writes
        active: sel.writes
        activeColour: sel.mode === "Write" ? Theme.record : Theme.caution
        onClicked: sel.open = !sel.open
    }

    Item {
        id: layer
        anchors.fill: parent
        visible: sel.open
        z: 1000

        // The press is taken here and goes no further.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: sel.open = false
            onWheel: sel.open = false
        }

        Rectangle {
            id: list
            width: Math.max(sel.width, 58)
            height: sel.modes.length * 18 + 4
            radius: Theme.radius
            color: Theme.panel
            border.color: Theme.borderStrong
            onActiveFocusChanged: {
                if (activeFocus) sel.shell.setTextEntry(true)
                else if (sel.open) sel.open = false
            }
            Keys.onPressed: (e) => {
                e.accepted = true
                switch (e.key) {
                case Qt.Key_Escape:
                    sel.open = false
                    return
                case Qt.Key_Up:
                    sel.step(-1)
                    return
                case Qt.Key_Down:
                    sel.step(1)
                    return
                case Qt.Key_Return:
                case Qt.Key_Enter:
                case Qt.Key_Space:
                    if (sel.hot >= 0) sel.choose(sel.modes[sel.hot])
                    else sel.open = false
                    return
                case Qt.Key_Shift:
                case Qt.Key_Control:
                case Qt.Key_Alt:
                case Qt.Key_Meta:
                    return
                }
                sel.open = false
                sel.shell.say("The automation mode list was open: that key closed it and did nothing else")
            }
            // A press on the list's edge is not a click-away.
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

            Column {
                anchors { fill: parent; margins: 2 }
                spacing: 0
                Repeater {
                    model: sel.modes
                    SButton {
                        required property string modelData
                        required property int index
                        // After the button's own name: a strip's and the Edit
                        // header's lists for one track are told apart.
                        objectName: sel.objectName.length > 0 ? sel.objectName + modelData : ""
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
                        onClicked: sel.choose(modelData)
                        // The keyboard's row, when it is not the current mode.
                        Rectangle {
                            anchors.fill: parent
                            visible: index === sel.hot && !parent.active
                            color: "transparent"
                            radius: Theme.radius
                            border.color: Theme.accent
                            border.width: 1
                        }
                    }
                }
            }
        }
    }
}
