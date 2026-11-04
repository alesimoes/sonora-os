// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip Gain: the selected clip's level in dB, typed. Asked for by clip.gain
// with no level (the key, or the Edit menu). Return sends the command with the
// number; a refusal (not a number, out of range) keeps the dialog open and says
// why inside it, until the next try or Cancel.
import QtQuick
import Sonora

Item {
    id: gainDialog
    property var shell: null
    property string problem: ""
    visible: false
    anchors.fill: parent
    z: 9

    function show(current) {
        problem = ""
        gainInput.text = current.toFixed(1)
        visible = true
        gainInput.forceActiveFocus()
        gainInput.selectAll()
    }
    function close() {
        visible = false
        gainInput.focus = false
        if (shell) shell.setTextEntry(false)
    }
    function accept() {
        if (shell.invokeWith("clip.gain", -1, gainInput.text)) close()
        else problem = shell.statusText
    }

    MouseArea {
        anchors.fill: parent
        onClicked: gainDialog.close()
    }
    Rectangle {
        objectName: "clipGainPanel"
        z: 10
        anchors.centerIn: parent
        width: 320
        height: gainColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: gainColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Clip Gain (dB)"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Rectangle {
                width: parent.width
                height: 26
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: gainInput.activeFocus ? Theme.accent : Theme.borderStrong
                TextInput {
                    id: gainInput
                    objectName: "clipGainInput"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    onActiveFocusChanged: if (gainDialog.shell) gainDialog.shell.setTextEntry(activeFocus)
                    onTextEdited: gainDialog.problem = ""
                    Keys.onEscapePressed: gainDialog.close()
                    Keys.onReturnPressed: gainDialog.accept()
                    Keys.onEnterPressed: gainDialog.accept()
                }
            }
            Text {
                objectName: "clipGainProblem"
                visible: gainDialog.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: gainDialog.problem
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "clipGainCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: gainDialog.close()
                }
                SButton {
                    objectName: "clipGainOk"
                    text: "Set"
                    implicitHeight: 24
                    onClicked: gainDialog.accept()
                }
            }
        }
    }
}
