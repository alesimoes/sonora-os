// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove (ADR-0115): pick a feel, set how far toward it, and move the
// selected regions onto it in one undo step. Every action is a tempo.edit
// "groove ..." text, the same words a key map or the Tempo dialog sends, so
// the panel adds no command of its own. A refusal, or regions left in place
// because they would overlap, stays inside the panel until the next try.
import QtQuick
import Sonora

Item {
    id: groovePanel
    property var shell: null
    // Names in the saved-grooves file; whoever opens the panel fills it.
    property var saved: []
    property int current: 2
    property real strength: 100
    property string problem: ""
    property string done: ""
    visible: false
    anchors.fill: parent
    z: 9

    // A swing preset is "percent subdivision"; the reference reads the top
    // selected track's hits.
    readonly property var presets: [
        { label: "Straight", detail: "50% sixteenths, back on the grid", words: "swing 50 16" },
        { label: "Light swing", detail: "56% sixteenths", words: "swing 56 16" },
        { label: "Pocket", detail: "62% sixteenths", words: "swing 62 16" },
        { label: "Hard shuffle", detail: "66% sixteenths, a triplet feel", words: "swing 66 16" },
        { label: "Eighth shuffle", detail: "66% eighths", words: "swing 66 8" },
        { label: "From reference", detail: "the top selected track's hits, 4 per beat", words: "region 4" }
    ]
    readonly property int entries: presets.length + saved.length

    function quoted(name) { return "\"" + name + "\"" }
    function wordsOf(i) {
        return i < presets.length ? presets[i].words : "saved " + quoted(saved[i - presets.length])
    }
    function show() {
        problem = ""
        done = ""
        visible = true
    }
    function close() {
        visible = false
        saveName.focus = false
        if (shell) shell.setTextEntry(false)
    }
    function run(text) {
        problem = ""
        done = ""
        if (!shell) return false
        if (shell.invokeWith("tempo.edit", -1, text)) return true
        problem = shell.statusText
        return false
    }
    function apply() {
        if (run("groove " + wordsOf(current) + " " + Math.round(strength))) close()
    }
    function saveAs() {
        const name = saveName.text.trim()
        if (name.length === 0) {
            problem = "type a name to save this groove under"
            return
        }
        if (current >= presets.length) {
            problem = "that groove is saved already; pick a preset or the reference to save"
            return
        }
        if (!run("groove save " + quoted(name) + " " + presets[current].words)) return
        if (saved.indexOf(name) < 0) saved = saved.concat([name])
        done = "Saved as " + name
    }

    MouseArea {
        anchors.fill: parent
        onClicked: groovePanel.close()
    }
    Rectangle {
        objectName: "groovePanel"
        z: 10
        anchors.centerIn: parent
        width: 420
        height: grooveColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: grooveColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space3

            Text {
                text: "Groove"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Moves the selected regions onto the feel below. One Undo puts them back."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }

            // The feels: presets, then the operator's saved ones.
            Rectangle {
                width: parent.width
                height: Math.min(groovePanel.entries, 9) * 34 + 2
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                ListView {
                    id: feelList
                    objectName: "grooveList"
                    anchors { fill: parent; margins: 1 }
                    clip: true
                    model: groovePanel.entries
                    delegate: Rectangle {
                        required property int index
                        readonly property bool isSaved: index >= groovePanel.presets.length
                        readonly property bool chosen: index === groovePanel.current
                        width: feelList.width
                        height: 34
                        color: chosen ? Theme.selectionFill : (rowMouse.containsMouse ? Theme.hover : "transparent")
                        Rectangle {
                            visible: chosen
                            width: 3
                            height: parent.height
                            color: Theme.accent
                        }
                        Text {
                            id: feelName
                            anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                            text: isSaved ? groovePanel.saved[index - groovePanel.presets.length]
                                          : groovePanel.presets[index].label
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: chosen ? Font.DemiBold : Font.Normal }
                        }
                        Text {
                            anchors { right: parent.right; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                            text: isSaved ? "saved" : groovePanel.presets[index].detail
                            color: isSaved ? Theme.accent : Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                        }
                        Rectangle {
                            visible: index === groovePanel.presets.length && groovePanel.saved.length > 0
                            width: parent.width
                            height: 1
                            color: Theme.borderStrong
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { groovePanel.current = index; groovePanel.problem = ""; groovePanel.done = "" }
                            onDoubleClicked: groovePanel.apply()
                        }
                    }
                }
            }

            // Strength: how far from where they are toward the groove.
            Item {
                width: parent.width
                height: 30
                Text {
                    id: strengthLabel
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: 70
                    text: "Strength"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Item {
                    id: strengthBar
                    objectName: "grooveStrength"
                    anchors { left: strengthLabel.right; right: strengthValue.left; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                    height: 22
                    readonly property real ratio: (groovePanel.strength - 1) / 99
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        height: 4
                        radius: 2
                        color: Theme.sunken
                        border.color: Theme.borderSubtle
                    }
                    Rectangle {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        width: strengthBar.ratio * strengthBar.width
                        height: 4
                        radius: 2
                        color: Theme.accent
                    }
                    Rectangle {
                        x: strengthBar.ratio * (strengthBar.width - width)
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        radius: 7
                        color: Theme.raised
                        border.color: Theme.accent
                        border.width: 2
                    }
                    MouseArea {
                        anchors.fill: parent
                        function set(x) {
                            groovePanel.strength = Math.round(1 + 99 * Math.max(0, Math.min(1, x / width)))
                        }
                        onPressed: (mouse) => set(mouse.x)
                        onPositionChanged: (mouse) => set(mouse.x)
                    }
                }
                Text {
                    id: strengthValue
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    width: 44
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(groovePanel.strength) + "%"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                }
            }

            // Save as: the chosen preset or reference, under the operator's name.
            Row {
                width: parent.width
                spacing: Theme.space2
                Rectangle {
                    width: parent.width - saveButton.width - Theme.space2
                    height: 24
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: saveName.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: saveName
                        objectName: "grooveSaveName"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                        onActiveFocusChanged: if (groovePanel.shell) groovePanel.shell.setTextEntry(activeFocus)
                        Keys.onEscapePressed: groovePanel.close()
                        Keys.onReturnPressed: groovePanel.saveAs()
                        Text {
                            visible: saveName.text.length === 0 && !saveName.activeFocus
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Name for this groove"
                            color: Theme.textTertiary
                            font: saveName.font
                        }
                    }
                }
                SButton {
                    id: saveButton
                    objectName: "grooveSave"
                    text: "Save As"
                    implicitHeight: 24
                    implicitWidth: 76
                    onClicked: groovePanel.saveAs()
                }
            }

            Text {
                objectName: "grooveProblem"
                visible: groovePanel.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: groovePanel.problem
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Text {
                visible: groovePanel.done.length > 0
                width: parent.width
                text: groovePanel.done
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "grooveCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    implicitWidth: 76
                    onClicked: groovePanel.close()
                }
                SButton {
                    objectName: "grooveApply"
                    text: "Apply"
                    active: true
                    implicitHeight: 24
                    implicitWidth: 76
                    onClicked: groovePanel.apply()
                }
            }
        }
    }
}
