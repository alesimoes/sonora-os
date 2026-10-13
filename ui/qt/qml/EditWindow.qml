// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window: toolbar, rulers, track headers, and the timeline.
// The timeline and rulers are scene-graph items (Sonora.Timeline,
// Sonora.Ruler); the headers are ordinary controls, which is what they are.
// Every gesture goes to SessionModel, and every button sends the command its
// key sends.
import QtQuick
import Sonora

Item {
    id: edit
    required property ShellModel shell
    required property SessionModel session

    // --- toolbar --------------------------------------------------------------
    Rectangle {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.toolbarHeight
        color: Theme.panel
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }

        Row {
            anchors { left: parent.left; leftMargin: Theme.space4; verticalCenter: parent.verticalCenter }
            spacing: Theme.space6

            // Edit modes, in the order an engineer's F1-F4 reach them.
            Row {
                spacing: 2
                Repeater {
                    model: [["Shuffle", "edit.mode.shuffle"], ["Spot", "edit.mode.spot"],
                            ["Slip", "edit.mode.slip"], ["Grid", "edit.mode.grid"]]
                    SButton {
                        required property var modelData
                        text: modelData[0]
                        implicitWidth: 60
                        active: edit.session.editMode === modelData[0]
                        onClicked: edit.shell.invoke(modelData[1], -1)
                    }
                }
            }
            Row {
                spacing: 2
                Repeater {
                    model: [["Smart", "tool.smart"], ["Trim", "tool.trim"],
                            ["Select", "tool.select"], ["Grab", "tool.grab"]]
                    SButton {
                        required property var modelData
                        text: modelData[0]
                        implicitWidth: 56
                        active: edit.session.tool === modelData[0]
                        onClicked: edit.shell.invoke(modelData[1], -1)
                    }
                }
            }
            Row {
                spacing: Theme.space1
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "GRID"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; letterSpacing: 0.6 }
                }
                SButton {
                    text: edit.session.gridValue
                    implicitWidth: 52
                    bold: false
                    onClicked: edit.session.cycleGrid()
                }
            }
            Row {
                spacing: 2
                SButton { text: "−"; implicitWidth: 28; fontPx: Theme.textTitle; onClicked: edit.shell.invoke("zoom.out", -1) }
                SButton { text: "+"; implicitWidth: 28; fontPx: Theme.textTitle; onClicked: edit.shell.invoke("zoom.in", -1) }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: Theme.space2
                    text: edit.session.zoomText
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                }
            }
            Row {
                spacing: Theme.space3
                Repeater {
                    model: [["START", edit.session.selectionStart], ["END", edit.session.selectionEnd],
                            ["LENGTH", edit.session.selectionLength]]
                    Column {
                        required property var modelData
                        Text {
                            text: modelData[0]
                            color: Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
                        }
                        Text {
                            text: modelData[1]
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                        }
                    }
                }
            }
        }
        Text {
            anchors { right: parent.right; rightMargin: Theme.space4; verticalCenter: parent.verticalCenter }
            text: edit.session.sessionName
            color: edit.session.synthetic ? Theme.caution : Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
    }

    // --- rulers ---------------------------------------------------------------
    Column {
        id: rulers
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom }
        Repeater {
            model: ["Timecode", "Bars|Beats", "Min:Secs"]
            Item {
                required property int index
                required property string modelData
                width: rulers.width
                height: Theme.rulerHeight
                Rectangle {
                    width: Theme.trackHeaderWidth; height: parent.height
                    color: Theme.panel
                    Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.borderSubtle }
                    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }
                    Text {
                        anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                        text: modelData
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
                    }
                }
                Ruler {
                    objectName: "ruler" + index
                    x: Theme.trackHeaderWidth
                    width: parent.width - Theme.trackHeaderWidth
                    height: parent.height
                    session: edit.session
                    format: index
                    MouseArea { anchors.fill: parent; onClicked: (m) => edit.session.rulerClick(m.x) }
                }
            }
        }
    }

    // --- tracks ---------------------------------------------------------------
    Item {
        id: tracks
        anchors { left: parent.left; right: parent.right; top: rulers.bottom; bottom: parent.bottom }
        clip: true

        // Headers scroll with the lanes, by the same offset.
        Item {
            id: headers
            width: Theme.trackHeaderWidth
            height: parent.height
            clip: true
            Repeater {
                model: edit.session.trackNames
                Rectangle {
                    id: header
                    required property int index
                    required property string modelData
                    readonly property bool selected: edit.session.selectedTrack === index
                    readonly property bool armed: edit.shell.engineAlive && edit.shell.armed[index] === true
                    y: index * edit.session.trackHeight - edit.session.scrollY
                    width: headers.width
                    height: edit.session.trackHeight
                    visible: y + height >= 0 && y <= headers.height
                    color: selected ? Theme.raised : Theme.panel

                    MouseArea { anchors.fill: parent; onClicked: edit.session.selectTrack(header.index) }
                    Rectangle {  // the track's colour, as its regions wear it
                        width: 3; height: parent.height - 2
                        color: Theme.trackColour(header.index)
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 1; color: Theme.borderSubtle
                    }
                    Rectangle {
                        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                        width: 1; color: Theme.borderSubtle
                    }
                    Text {
                        x: Theme.space3; y: Theme.space2
                        width: parent.width - 2 * Theme.space3 - 14
                        elide: Text.ElideRight
                        text: header.modelData
                        color: header.selected ? Theme.text : Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                    }
                    Row {
                        x: Theme.space3
                        anchors { bottom: parent.bottom; bottomMargin: Theme.space2 }
                        spacing: Theme.space1
                        SButton {
                            text: "R"; implicitWidth: 24; implicitHeight: 20
                            objectName: "arm" + header.index
                            active: header.armed
                            activeColour: Theme.record
                            available: edit.shell.engineAlive
                            onClicked: edit.shell.invoke("track.toggle_arm", header.index)
                        }
                        SButton {
                            text: "S"; implicitWidth: 24; implicitHeight: 20
                            active: edit.session.solos[header.index] === true
                            activeColour: Theme.solo
                            onClicked: edit.shell.invoke("track.toggle_solo", header.index)
                        }
                        SButton {
                            text: "M"; implicitWidth: 24; implicitHeight: 20
                            active: edit.session.mutes[header.index] === true
                            activeColour: Theme.mute
                            onClicked: edit.shell.invoke("track.toggle_mute", header.index)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            leftPadding: Theme.space2
                            text: edit.session.gainText(edit.session.gains[header.index] || 0) + " dB"
                            color: Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                        }
                    }
                    Meter {
                        objectName: "editMeter" + header.index
                        source: edit.shell
                        channel: header.index
                        anchors { right: parent.right; rightMargin: Theme.space2; top: parent.top; bottom: parent.bottom; margins: Theme.space1 }
                        width: 6
                    }
                }
            }
        }

        Timeline {
            id: timeline
            objectName: "timeline"
            x: Theme.trackHeaderWidth
            width: parent.width - Theme.trackHeaderWidth
            height: parent.height
            session: edit.session

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: (m) => edit.session.press(m.x, m.y)
                onPositionChanged: (m) => { if (pressed) edit.session.drag(m.x, m.y) }
                onReleased: (m) => edit.session.release(m.x, m.y)
                onWheel: (w) => edit.session.wheel(w.x, w.angleDelta.x / 3, w.angleDelta.y / 3,
                                                   (w.modifiers & Qt.ShiftModifier) !== 0,
                                                   (w.modifiers & Qt.ControlModifier) !== 0)
            }
        }
    }
}
