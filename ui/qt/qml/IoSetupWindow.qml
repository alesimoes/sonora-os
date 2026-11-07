// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > I/O Setup (Pro Tools Reference Guide, I/O Setup): the hardware
// inputs and outputs by name on the left, each audio track's input picked
// from those names on the right (IoSetupModel). A name is typed in place
// and kept for this machine; Return keeps it, Escape takes it back. A track
// whose input is not connected says so in its row, in the alarm colour: a
// meter at zero there is not silence. A refusal stays in the window until
// the next action.
//
// Output paths (ADR-0060) are named below the outputs: each plays on a pair
// of them, and is picked as a strip's output in the Mix window like a bus.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property IoSetupModel io
    property Item mover: win
    // The track row whose input list is open; -1: none.
    property int picking: -1
    // The output path whose pair list is open; -1: none.
    property int pathPicking: -1
    readonly property int rowHeight: 24
    readonly property int listRows: 18

    width: 760
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: io.open
    onVisibleChanged: if (!visible) { picking = -1; pathPicking = -1 }

    MouseArea { anchors.fill: parent; onClicked: { win.picking = -1; win.pathPicking = -1 } }

    Rectangle {
        anchors { fill: parent; topMargin: 4; leftMargin: 4; rightMargin: -4; bottomMargin: -4 }
        radius: Theme.radius
        color: Theme.shadow
        z: -1
    }

    component Caption: Text {
        color: Theme.textTertiary
        font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
    }

    // One hardware port: its number, and its name, typed in place.
    component PortRow: Item {
        id: port
        property int slot: 0
        property string name
        property bool renamed: false
        property bool input: true
        width: parent ? parent.width : 0
        height: win.rowHeight
        Text {
            width: 36
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignRight
            text: port.slot + 1
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
        }
        Rectangle {
            x: 44
            width: parent.width - 44
            height: win.rowHeight - 4
            anchors.verticalCenter: parent.verticalCenter
            radius: Theme.radiusSmall
            color: field.activeFocus ? Theme.window : Theme.sunken
            border.color: field.activeFocus ? Theme.accent : Theme.borderSubtle
            TextInput {
                id: field
                objectName: (port.input ? "ioInputName" : "ioOutputName") + port.slot
                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                text: port.name
                maximumLength: 32
                color: port.renamed ? Theme.text : Theme.textSecondary
                selectionColor: Theme.selectionFill
                selectedTextColor: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                function keep() {
                    if (text === port.name) return
                    const ok = port.input ? win.io.renameInput(port.slot, text)
                                          : win.io.renameOutput(port.slot, text)
                    // Refused, or trimmed: the field shows what is kept.
                    text = Qt.binding(() => port.name)
                    if (ok) focus = false
                }
                onActiveFocusChanged: {
                    win.shell.setTextEntry(activeFocus)
                    if (!activeFocus) keep()
                }
                Keys.onReturnPressed: keep()
                Keys.onEnterPressed: keep()
                Keys.onEscapePressed: {
                    text = Qt.binding(() => port.name)
                    focus = false
                }
            }
        }
    }

    Column {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space3 }
        spacing: Theme.space2

        // --- title: drag to move ---
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
                text: "I/O Setup"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "ioSetupClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.io.open = false
            }
        }

        Text {
            objectName: "ioSetupWhat"
            width: parent.width
            wrapMode: Text.Wrap
            text: win.io.known
                  ? "Name what is plugged into each input, then pick a track's input by its name. "
                    + "The names are kept for this machine; which input a track records is saved with the session."
                  : "The engine is not answering, so which inputs this interface has is not known. "
                    + "Nothing is listed until it answers."
            color: win.io.known ? Theme.textSecondary : Theme.caution
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }

        Row {
            width: parent.width
            spacing: Theme.space4

            // --- the hardware ---
            Column {
                width: 300
                spacing: Theme.space1
                Caption {
                    objectName: "ioInputsCaption"
                    text: "INPUTS" + (win.io.known ? "  ·  " + win.io.inputs.length + " CONNECTED" : "")
                }
                Flickable {
                    id: inputList
                    width: parent.width
                    height: Math.min(win.io.inputs.length, win.listRows) * win.rowHeight
                    contentHeight: inputColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Column {
                        id: inputColumn
                        width: inputList.width
                        Repeater {
                            model: win.io.inputs
                            PortRow {
                                required property var modelData
                                slot: modelData.number - 1
                                name: modelData.name
                                renamed: modelData.renamed
                                input: true
                            }
                        }
                    }
                }
                Item { width: 1; height: Theme.space2 }
                Caption {
                    text: "OUTPUTS" + (win.io.outputs.length > 0 ? "  ·  " + win.io.outputs.length + " OPEN" : "")
                }
                Repeater {
                    model: win.io.outputs
                    PortRow {
                        required property var modelData
                        slot: modelData.number - 1
                        name: modelData.name
                        renamed: modelData.renamed
                        input: false
                    }
                }
                Text {
                    visible: win.io.outputs.length === 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: "The engine has not said which outputs it opened."
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Item { width: 1; height: Theme.space2 }
                Row {
                    spacing: Theme.space2
                    Caption {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "OUTPUT PATHS" + (win.io.paths.length > 0 ? "  ·  " + win.io.paths.length : "")
                    }
                    SButton {
                        objectName: "ioPathNew"
                        implicitHeight: 20
                        bold: false
                        fontPx: Theme.textCaption
                        text: "New Path"
                        available: win.io.outputs.length >= 2
                        onClicked: {
                            win.pathPicking = -1
                            // The first pair nobody plays on.
                            for (let i = 0; i < win.io.pairs.length; ++i) {
                                if (!win.io.pairs[i].taken) {
                                    win.io.addPath("", win.io.pairs[i].pair)
                                    return
                                }
                            }
                            win.io.addPath("", -1)
                        }
                    }
                }
                Repeater {
                    model: win.io.paths
                    Column {
                        id: pathItem
                        required property var modelData
                        width: parent.width
                        spacing: 2
                        Row {
                            spacing: Theme.space1
                            Rectangle {
                                width: 110
                                height: win.rowHeight - 4
                                radius: Theme.radiusSmall
                                color: pathName.activeFocus ? Theme.window : Theme.sunken
                                border.color: pathName.activeFocus ? Theme.accent : Theme.borderSubtle
                                TextInput {
                                    id: pathName
                                    objectName: "ioPathName" + pathItem.modelData.row
                                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                                    verticalAlignment: TextInput.AlignVCenter
                                    clip: true
                                    text: pathItem.modelData.name
                                    maximumLength: 64
                                    color: Theme.text
                                    selectionColor: Theme.selectionFill
                                    selectedTextColor: Theme.text
                                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                                    function keep() {
                                        if (text === pathItem.modelData.name) return
                                        const ok = win.io.renamePath(pathItem.modelData.row, text)
                                        text = Qt.binding(() => pathItem.modelData.name)
                                        if (ok) focus = false
                                    }
                                    onActiveFocusChanged: {
                                        win.shell.setTextEntry(activeFocus)
                                        if (!activeFocus) keep()
                                    }
                                    Keys.onReturnPressed: keep()
                                    Keys.onEnterPressed: keep()
                                    Keys.onEscapePressed: {
                                        text = Qt.binding(() => pathItem.modelData.name)
                                        focus = false
                                    }
                                }
                            }
                            SButton {
                                objectName: "ioPathPair" + pathItem.modelData.row
                                width: 140
                                implicitHeight: win.rowHeight - 4
                                alignLeft: true
                                bold: false
                                text: pathItem.modelData.pairText + "  ▾"
                                active: win.pathPicking === pathItem.modelData.row
                                onClicked: {
                                    win.io.dismissRefusal()
                                    win.pathPicking = win.pathPicking === pathItem.modelData.row ? -1 : pathItem.modelData.row
                                }
                            }
                            SButton {
                                objectName: "ioPathRemove" + pathItem.modelData.row
                                implicitWidth: 22
                                implicitHeight: win.rowHeight - 4
                                bold: false
                                text: "×"
                                onClicked: { win.pathPicking = -1; win.io.removePath(pathItem.modelData.row) }
                            }
                        }
                        Text {
                            readonly property var path: pathItem.modelData
                            visible: text.length > 0
                            width: parent.width
                            wrapMode: Text.Wrap
                            text: path.note.length > 0 ? path.note
                                  : path.strips > 0 ? path.strips + " strip" + (path.strips === 1 ? "" : "s") + " routed here"
                                  : ""
                            color: path.note.length > 0 ? Theme.alarm : Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                        }
                        // The pairs of outputs, under the path being moved.
                        Flow {
                            visible: win.pathPicking === pathItem.modelData.row
                            width: parent.width
                            spacing: Theme.space1
                            Repeater {
                                model: win.io.pairs
                                SButton {
                                    required property var modelData
                                    objectName: "ioPathPick" + modelData.pair
                                    implicitHeight: 20
                                    bold: false
                                    fontPx: Theme.textCaption
                                    text: modelData.text + (modelData.by.length > 0 ? "  (" + modelData.by + ")" : "")
                                    available: !modelData.taken
                                    onClicked: {
                                        const row = win.pathPicking
                                        win.pathPicking = -1
                                        win.io.setPathPair(row, modelData.pair)
                                    }
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: win.io.paths.length === 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: "None. A path plays a bus on a pair of outputs of its own, such as headphones."
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }

            // --- each track's input ---
            Column {
                width: parent.width - 300 - Theme.space4
                spacing: Theme.space1
                Caption { text: "TRACK INPUTS" }
                Flickable {
                    id: trackList
                    width: parent.width
                    height: Math.min(win.io.tracks.length, win.listRows) * win.rowHeight
                    contentHeight: trackColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Column {
                        id: trackColumn
                        width: trackList.width
                        Repeater {
                            model: win.io.tracks
                            Item {
                                required property var modelData
                                width: trackColumn.width
                                height: win.rowHeight
                                Text {
                                    width: 120
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    text: modelData.name
                                    color: Theme.text
                                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                                }
                                SButton {
                                    objectName: "ioTrackInput" + modelData.row
                                    x: 128
                                    width: 150
                                    implicitHeight: win.rowHeight - 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    alignLeft: true
                                    bold: false
                                    text: modelData.inputText + (modelData.audio ? "  ▾" : "")
                                    available: modelData.audio && win.io.known
                                    active: win.picking === modelData.row
                                    onClicked: {
                                        win.io.dismissRefusal()
                                        if (!modelData.audio) {
                                            win.io.setTrackInput(modelData.row, 0)
                                            return
                                        }
                                        win.picking = win.picking === modelData.row ? -1 : modelData.row
                                    }
                                }
                                Text {
                                    objectName: "ioTrackNote" + modelData.row
                                    x: 286
                                    width: parent.width - 286
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    text: modelData.note
                                    color: Theme.alarm
                                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            objectName: "ioSetupRefusal"
            visible: text.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            text: win.io.refusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
    }

    // --- the input list of one track, under its button ---
    Rectangle {
        id: picker
        objectName: "ioInputPicker"
        visible: win.picking >= 0
        z: 5
        readonly property var row: win.picking >= 0 && win.picking < win.io.tracks.length
                                   ? win.io.tracks[win.picking] : null
        width: 3 * 120 + 2 * Theme.space1 + 2 * Theme.space2
        height: pickerColumn.implicitHeight + 2 * Theme.space2
        radius: Theme.radius
        color: Theme.raised
        border.color: Theme.borderStrong
        x: Math.min(win.width - width - Theme.space2, trackList.mapToItem(win, 128, 0).x)
        y: {
            const top = trackList.mapToItem(win, 0, (win.picking + 1) * win.rowHeight - trackList.contentY).y
            return Math.min(win.height - height - Theme.space2, top)
        }
        MouseArea { anchors.fill: parent }
        Column {
            id: pickerColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space2 }
            spacing: Theme.space1
            Caption {
                text: picker.row ? (picker.row.channels > 1
                                    ? picker.row.name.toUpperCase() + " RECORDS " + picker.row.channels
                                      + " INPUTS FROM"
                                    : picker.row.name.toUpperCase() + " RECORDS FROM")
                                 : ""
            }
            Grid {
                columns: 3
                spacing: Theme.space1
                Repeater {
                    model: win.io.inputs
                    SButton {
                        required property var modelData
                        objectName: "ioPick" + (modelData.number - 1)
                        width: 120
                        implicitHeight: 22
                        alignLeft: true
                        bold: false
                        text: modelData.number + "  " + modelData.name
                        active: picker.row !== null && picker.row.input === modelData.number - 1
                        onClicked: {
                            const row = win.picking
                            win.picking = -1
                            win.io.setTrackInput(row, modelData.number - 1)
                        }
                    }
                }
            }
        }
    }
}
