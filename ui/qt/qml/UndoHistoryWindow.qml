// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Undo History window: Pro Tools' Window > Undo History, floating over
// the Edit or Mix window. Every step undo can reach, oldest first, newest at
// the bottom; the steps redo would bring back in italics below the line.
// A click on a done step undoes it and every later one; a click on an
// undone step redoes up to it, each step its own Undo or Redo, as the keys
// would send them.
//
// The window says when older steps have gone (the history is bounded by
// bytes, ADR-0009 §1), so the top of the list is never mistaken for the
// session as it was opened.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    // What a drag on the title moves: the item holding the window.
    property Item mover: win
    readonly property int rowHeight: 24
    readonly property int shownRows: 12

    width: 300
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.undoHistoryOpen

    // Taken by the window: a click on it never reaches the timeline beneath.
    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors { fill: parent; topMargin: 4; leftMargin: 4; rightMargin: -4; bottomMargin: -4 }
        radius: Theme.radius
        color: Theme.shadow
        z: -1
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
                text: "Undo History"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "undoHistoryClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.shell.invoke("window.undo_history", -1)
            }
        }

        Rectangle {
            width: parent.width
            height: win.rowHeight * win.shownRows + 2
            radius: Theme.radiusSmall
            color: Theme.sunken
            border.color: Theme.borderSubtle

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "Nothing to undo"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }

            ListView {
                id: list
                objectName: "undoHistoryList"
                anchors { fill: parent; margins: 1 }
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: win.session.undoHistory
                // The newest done step in view, where the operator is.
                onCountChanged: positionViewAtIndex(Math.max(0, lastDone()), ListView.Contain)
                function lastDone() {
                    const rows = win.session.undoHistory
                    let at = -1
                    for (let i = 0; i < rows.length; ++i) if (rows[i].done) at = i
                    return at
                }
                delegate: Rectangle {
                    id: row
                    objectName: "undoHistoryRow"
                    required property var modelData
                    required property int index
                    readonly property bool current: index === list.lastDone()
                    width: ListView.view.width
                    height: win.rowHeight
                    color: current ? Theme.selectionFill
                         : area.containsMouse ? Theme.hover : "transparent"
                    // The line between what is done and what redo would bring back.
                    Rectangle {
                        visible: row.current && row.index + 1 < list.count
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 1
                        color: Theme.accent
                    }
                    Text {
                        id: number
                        anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                        width: 26
                        text: row.index + 1
                        color: Theme.textTertiary
                        horizontalAlignment: Text.AlignRight
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                    }
                    Text {
                        objectName: "undoHistoryLabel"
                        anchors { left: number.right; leftMargin: Theme.space3; right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                        text: row.modelData.label
                        elide: Text.ElideRight
                        color: row.modelData.done ? Theme.text : Theme.textTertiary
                        font {
                            family: Theme.fontFamily; pixelSize: Theme.textBody
                            italic: !row.modelData.done
                            weight: row.modelData.done ? Font.DemiBold : Font.Normal
                        }
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: win.session.undoHistoryClick(row.index)
                    }
                }
            }
        }

        Row {
            spacing: Theme.space2
            SButton {
                objectName: "undoHistoryUndoAll"
                text: "Undo All"; implicitWidth: 90; fontPx: Theme.textSmall
                available: win.session.canUndo
                onClicked: win.session.undoHistoryAll(true)
            }
            SButton {
                objectName: "undoHistoryRedoAll"
                text: "Redo All"; implicitWidth: 90; fontPx: Theme.textSmall
                available: win.session.canRedo
                onClicked: win.session.undoHistoryAll(false)
            }
        }

        Text {
            objectName: "undoHistoryNote"
            width: parent.width
            wrapMode: Text.Wrap
            text: win.session.undoHistoryTrimmed
                  ? "Older steps were dropped as the history filled: undo goes no further back than step 1."
                  : "Back to the session as it was opened. Not kept when the session closes."
            color: win.session.undoHistoryTrimmed ? Theme.caution : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
