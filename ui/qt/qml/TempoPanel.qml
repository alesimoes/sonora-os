// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tempo: the tempo map edited by typing, at the insertion point or on the
// selected range. Asked for by tempo.edit with no text (the Edit menu).
// Return sends the command; a refusal keeps the dialog open and says why
// inside it, until the next try or Cancel.
import QtQuick
import Sonora

Item {
    id: tempoDialog
    property var shell: null
    property string problem: ""
    visible: false
    anchors.fill: parent
    z: 9

    function show(current) {
        problem = ""
        tempoInput.text = current
        visible = true
        tempoInput.forceActiveFocus()
        tempoInput.selectAll()
    }
    function close() {
        visible = false
        tempoInput.focus = false
        if (shell) shell.setTextEntry(false)
    }
    function accept() {
        if (shell.invokeWith("tempo.edit", -1, tempoInput.text)) close()
        else problem = shell.statusText
    }

    MouseArea {
        anchors.fill: parent
        onClicked: tempoDialog.close()
    }
    Rectangle {
        objectName: "tempoPanel"
        z: 10
        anchors.centerIn: parent
        width: 360
        height: tempoColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: tempoColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Tempo"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Rectangle {
                width: parent.width
                height: 26
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: tempoInput.activeFocus ? Theme.accent : Theme.borderStrong
                TextInput {
                    id: tempoInput
                    objectName: "tempoInput"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    onActiveFocusChanged: if (tempoDialog.shell) tempoDialog.shell.setTextEntry(activeFocus)
                    onTextEdited: tempoDialog.problem = ""
                    Keys.onEscapePressed: tempoDialog.close()
                    Keys.onReturnPressed: tempoDialog.accept()
                    Keys.onEnterPressed: tempoDialog.accept()
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "96 sets the tempo here.  120>90 ramps over the selection.  x2 doubles " +
                      "it over the selection.  3/4 starts a 3/4 bar.  remove deletes the " +
                      "change nearest the cursor."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Text {
                objectName: "tempoProblem"
                visible: tempoDialog.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: tempoDialog.problem
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "tempoCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: tempoDialog.close()
                }
                SButton {
                    objectName: "tempoOk"
                    text: "Apply"
                    implicitHeight: 24
                    onClicked: tempoDialog.accept()
                }
            }
        }
    }
}
