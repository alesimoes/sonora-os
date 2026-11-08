// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Preferences: what the operator starts with on this machine
// (counter format, edit mode, grid), kept in a file of its own, apart from
// every session. The audio format and the folders are the engine's and the
// interface's to report, so they are shown, not offered: a control that
// looked like it changed the buffer and did not would be worse than none.
// A refusal stays in the window until the next choice.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    property Item mover: win

    width: 440
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.preferencesOpen

    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors { fill: parent; topMargin: 4; leftMargin: 4; rightMargin: -4; bottomMargin: -4 }
        radius: Theme.radius
        color: Theme.shadow
        z: -1
    }

    component Overline: Text {
        color: Theme.textTertiary
        font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
    }

    // A label and a read-only value.
    component Fact: Row {
        id: fact
        property string label
        property string value
        spacing: Theme.space2
        width: parent.width
        Text {
            width: 96
            text: fact.label
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Text {
            width: fact.width - 96 - Theme.space2
            text: fact.value
            elide: Text.ElideMiddle
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
        }
    }

    // One button per choice; the one in force is lit.
    component Choice: Row {
        id: choice
        property string label
        property string what
        property var options: []
        property string current
        spacing: Theme.space1
        Text {
            width: 96
            anchors.verticalCenter: parent.verticalCenter
            text: choice.label
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Repeater {
            model: choice.options
            SButton {
                required property string modelData
                objectName: "pref_" + choice.what + "_" + modelData
                text: modelData
                implicitWidth: 72; implicitHeight: 24
                active: choice.current === modelData
                onClicked: win.session.setPreference(choice.what, modelData)
            }
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
                text: "Preferences"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "preferencesClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.shell.invoke("window.preferences", -1)
            }
        }

        // Facts only: the engine sets them when it starts, and a row appears
        // only when its value exists (no device or buffer control, ADR-0075).
        Overline { text: "PLAYBACK ENGINE (SET WHEN THE ENGINE STARTS)" }
        Fact {
            objectName: "prefAudio"; label: "Format"; value: win.shell.formatText
            visible: value.length > 0
        }
        Fact {
            objectName: "prefInputs"; label: "Inputs"; value: win.shell.trackCount + " tracks"
            visible: win.shell.trackCount > 0
        }

        Overline { text: "CLICK / COUNT OFF (AT START)" }
        Choice {
            objectName: "prefClick"
            label: "Click"; what: "click"
            options: ["on", "off"]
            current: win.session.prefClick ? "on" : "off"
        }
        Choice {
            objectName: "prefCountOff"
            label: "Count off"; what: "countoff"
            options: ["on", "off"]
            current: win.session.prefCountOff ? "on" : "off"
        }

        Overline { text: "RECORD PATHS" }
        Fact { objectName: "prefSessions"; label: "Sessions"; value: win.session.sessionsFolder() }
        Fact { objectName: "prefTemplates"; label: "Templates"; value: win.session.templatesFolder() }

        Overline { text: "DISPLAY" }
        Choice {
            objectName: "prefCounter"
            label: "Main counter"; what: "counter"
            options: ["Bars|Beats", "Min:Secs", "Timecode", "Samples"]
            current: win.session.prefCounter
        }

        Overline { text: "EDITING" }
        Choice {
            objectName: "prefMode"
            label: "Edit mode"; what: "mode"
            options: ["Shuffle", "Slip", "Spot", "Grid"]
            current: win.session.prefMode
        }
        Choice {
            objectName: "prefGrid"
            label: "Grid"; what: "grid"
            options: ["1 bar", "1/4", "1/8", "1/16"]
            current: win.session.prefGrid
        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Kept in " + win.session.preferencesFile + ", for every session."
            visible: win.session.preferencesFile.length > 0
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Text {
            objectName: "prefRefusal"
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: win.session.prefRefusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
    }
}
