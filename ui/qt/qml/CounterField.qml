// SPDX-License-Identifier: GPL-3.0-or-later
//
// A main counter that takes a position typed, as Pro Tools' does: a click
// on the digits opens it with the reading selected, Return or Enter locates
// there (SessionModel::enterCounter), Escape or a click elsewhere leaves
// the transport where it was. Text that is not a position keeps the field
// open and outlined in the alarm colour, so it can be corrected, with the
// reason in `reason` for the caption to hold. While it is open, keys go to it and not to the
// profile (ShellModel::setTextEntry): typing "1" must not pick a tool.
import QtQuick
import Sonora

Item {
    id: field
    required property ShellModel shell
    required property SessionModel session
    property string text
    property color textColour: Theme.text
    property int fontPx: Theme.textCounter
    readonly property bool editing: input.visible
    // The last Return was refused, and why; held, for the caption beside
    // the field, until the next keystroke, not the status line's moment.
    property bool refused: false
    property string reason

    Connections {
        target: field.session
        enabled: input.visible
        function onRefused(why) { field.reason = why }
    }

    implicitWidth: shown.implicitWidth
    implicitHeight: shown.implicitHeight

    function begin() {
        input.text = field.text
        field.refused = false
        input.visible = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function finish() {
        input.visible = false
        input.focus = false
        field.refused = false
        field.shell.setTextEntry(false)
    }
    function submit() {
        if (field.session.enterCounter(input.text)) field.finish()
        else field.refused = true
    }

    Text {
        id: shown
        visible: !input.visible
        text: field.text
        color: field.textColour
        font { family: Theme.fontFamily; pixelSize: field.fontPx; weight: Font.Medium; features: { "tnum": 1 } }
    }
    MouseArea {
        anchors.fill: parent
        enabled: !input.visible
        cursorShape: Qt.IBeamCursor
        onClicked: field.begin()
    }
    Rectangle {
        anchors { fill: parent; leftMargin: -4; rightMargin: -4 }
        visible: input.visible
        radius: Theme.radiusSmall
        color: Theme.window
        border.color: field.refused ? Theme.alarm : Theme.accent
    }
    TextInput {
        id: input
        objectName: field.objectName.length > 0 ? field.objectName + "Input" : ""
        anchors.fill: parent
        visible: false
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        color: field.refused ? Theme.alarm : Theme.text
        selectionColor: Theme.selectionFill
        selectedTextColor: Theme.text
        font { family: Theme.fontFamily; pixelSize: field.fontPx; weight: Font.Medium; features: { "tnum": 1 } }
        onTextEdited: field.refused = false
        onActiveFocusChanged: {
            field.shell.setTextEntry(activeFocus)
            if (!activeFocus && visible) {
                visible = false
                field.refused = false
            }
        }
        Keys.onEscapePressed: field.finish()
        Keys.onReturnPressed: field.submit()
        Keys.onEnterPressed: field.submit()
    }
}
