// SPDX-License-Identifier: GPL-3.0-or-later
//
// A readout that takes a typed value: a click puts the value in a field,
// selected; Return or Enter emits `submitted`, Escape or a click elsewhere
// leaves it as it was. While the field has the keyboard, keys go to it and
// not to the profile (ShellModel::setTextEntry): typing "-6" must not nudge
// a region. Text that is not a value leaves the field open, so it can be
// corrected; the reason is already on the status line.
import QtQuick
import Sonora

Item {
    id: field
    required property ShellModel shell
    property string text
    property color textColour: Theme.text
    property int fontPx: Theme.textSmall
    property bool editable: true
    // Return or Enter: the owner sets the value and calls finish(), or
    // leaves the field open on text that is not one.
    signal submitted(string text)
    readonly property bool editing: input.visible

    function begin() {
        if (!editable) return
        input.text = field.text
        input.visible = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function finish() {
        input.visible = false
        input.focus = false
        field.shell.setTextEntry(false)
    }

    Text {
        anchors.centerIn: parent
        visible: !input.visible
        text: field.text
        color: field.textColour
        font { family: Theme.fontFamily; pixelSize: field.fontPx; features: { "tnum": 1 } }
    }
    MouseArea {
        anchors.fill: parent
        enabled: !input.visible
        cursorShape: field.editable ? Qt.IBeamCursor : Qt.ArrowCursor
        onClicked: field.begin()
    }
    Rectangle {
        anchors.fill: parent
        visible: input.visible
        radius: Theme.radiusSmall
        color: Theme.window
        border.color: Theme.accent
    }
    TextInput {
        id: input
        objectName: field.objectName.length > 0 ? field.objectName + "Input" : ""
        anchors { fill: parent; leftMargin: 2; rightMargin: 2 }
        visible: false
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        color: Theme.text
        selectionColor: Theme.selectionFill
        selectedTextColor: Theme.text
        font { family: Theme.fontFamily; pixelSize: field.fontPx; features: { "tnum": 1 } }
        onActiveFocusChanged: {
            field.shell.setTextEntry(activeFocus)
            if (!activeFocus && visible) visible = false
        }
        Keys.onEscapePressed: field.finish()
        Keys.onReturnPressed: field.submitted(text)
        Keys.onEnterPressed: field.submitted(text)
    }
}
