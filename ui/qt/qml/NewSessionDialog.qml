// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > New Session (Pro Tools Reference Guide, Sessions): name, where it
// goes, file type, sample rate and bit depth. Rate and depth are this
// system's: the others are listed dimmed, and a click on one says why it
// cannot be had here instead of making a session that would not use it
// (SessionModel::newSessionChoices). A refused name keeps the dialog open
// with the reason in it, held until the next try.
import QtQuick
import Sonora

Rectangle {
    id: dlg
    required property ShellModel shell
    required property SessionModel session
    property var choices: ({ name: "", where: "", rates: [], depths: [] })
    property string problem
    property bool discard: false

    visible: false
    width: 460
    height: dlgColumn.implicitHeight + 2 * Theme.space4
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    MouseArea { anchors.fill: parent }

    function show() {
        choices = session.newSessionChoices()
        problem = ""
        discard = false
        nameInput.text = choices.name
        visible = true
        nameInput.forceActiveFocus()
        nameInput.selectAll()
    }
    function close() {
        visible = false
        nameInput.focus = false
        shell.setTextEntry(false)
    }
    function accept() {
        const why = session.newSession(nameInput.text, discard)
        if (why.length === 0) close()
        else problem = why
    }

    component Label: Text {
        width: 96
        color: Theme.text
        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
    }
    // The values one setting can take, this system's lit.
    component Choices: Row {
        id: row
        property var items: []
        property string name
        spacing: Theme.space1
        Repeater {
            model: row.items
            SButton {
                required property var modelData
                required property int index
                objectName: row.name + index
                text: modelData.text
                implicitHeight: 24
                bold: false
                active: modelData.on
                available: modelData.on
                onClicked: dlg.problem = modelData.on ? "" : modelData.text + ": " + modelData.why
            }
        }
    }

    Column {
        id: dlgColumn
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
        spacing: Theme.space2

        Text {
            text: "New Session"
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "An empty session with " + dlg.session.trackCount + " tracks, written to its own folder "
                  + "as soon as it is made."
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Row {
            spacing: Theme.space2
            Label { anchors.verticalCenter: parent.verticalCenter; text: "Session name" }
            Rectangle {
                width: 300
                height: 26
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: nameInput.activeFocus ? Theme.accent : Theme.borderStrong
                TextInput {
                    id: nameInput
                    objectName: "newSessionName"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    onActiveFocusChanged: dlg.shell.setTextEntry(activeFocus)
                    onTextChanged: dlg.problem = ""
                    Keys.onEscapePressed: dlg.close()
                    Keys.onReturnPressed: dlg.accept()
                    Keys.onEnterPressed: dlg.accept()
                }
            }
        }
        Row {
            spacing: Theme.space2
            Label { text: "Location" }
            Text {
                objectName: "newSessionWhere"
                width: 300
                elide: Text.ElideMiddle
                text: dlg.choices.where
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
        }
        Row {
            spacing: Theme.space2
            Label { text: "File type" }
            Text {
                text: "BWF (.wav)"
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
        }
        Row {
            spacing: Theme.space2
            Label { anchors.verticalCenter: parent.verticalCenter; text: "Sample rate" }
            Choices { objectName: "newSessionRates"; name: "newSessionRate"; items: dlg.choices.rates }
        }
        Row {
            spacing: Theme.space2
            Label { anchors.verticalCenter: parent.verticalCenter; text: "Bit depth" }
            Choices { objectName: "newSessionDepths"; name: "newSessionDepth"; items: dlg.choices.depths }
        }
        // Open's "Discard edits": without it a dirty session refuses.
        SButton {
            objectName: "newSessionDiscard"
            visible: dlg.session.dirty
            text: (dlg.discard ? "☑" : "☐") + "  Discard this session's unsaved edits"
            bold: false
            implicitHeight: 24
            onClicked: dlg.discard = !dlg.discard
        }
        Text {
            objectName: "newSessionProblem"
            visible: dlg.problem.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            text: dlg.problem
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Row {
            anchors.right: parent.right
            spacing: Theme.space2
            SButton {
                objectName: "newSessionCancel"
                text: "Cancel"
                implicitHeight: 24
                onClicked: dlg.close()
            }
            SButton {
                objectName: "newSessionCreate"
                text: "Create"
                implicitHeight: 24
                active: true
                onClicked: dlg.accept()
            }
        }
    }
}
