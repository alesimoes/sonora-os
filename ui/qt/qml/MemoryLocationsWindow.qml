// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Memory Locations window: Pro Tools' Window > Memory Locations,
// floating over the Edit or Mix window. Every marker of the session in time
// order, numbered; a selection marker shows its range. A click on a row
// recalls it (the selection moves there), the row's × clears it, one undo
// step; New adds one where marker.add would, through the same command.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    // What a drag on the title moves: the item holding the window.
    property Item mover: win
    readonly property int rowHeight: 24
    readonly property int shownRows: 8

    width: 480
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.memoryLocationsOpen

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
                text: "Memory Locations"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "memoryLocationsClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.shell.invoke("window.memory_locations", -1)
            }
        }

        // Column captions over the list.
        Item {
            width: parent.width
            height: 14
            Repeater {
                model: [["#", 2], ["NAME", 40], ["START", 196], ["END", 290], ["LENGTH", 376]]
                Text {
                    required property var modelData
                    x: modelData[1]
                    text: modelData[0]
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
                }
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
                width: parent.width - 2 * Theme.space4
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: list.count === 0
                text: "No markers. New, or the Markers ruler's +, adds one at the insertion point, or over the selection."
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }

            ListView {
                id: list
                objectName: "memoryLocationsList"
                anchors { fill: parent; margins: 1 }
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: win.session.markers
                delegate: Rectangle {
                    id: row
                    objectName: "memoryLocationRow"
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: win.rowHeight
                    color: area.containsMouse ? Theme.hover : "transparent"
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: win.session.recallMarker(row.index)
                    }
                    Rectangle {  // a flag, as the ruler draws it; a bar under it for a range
                        x: Theme.space1; anchors.verticalCenter: parent.verticalCenter
                        width: 7; height: 10
                        color: Theme.marker
                        opacity: row.modelData.range ? 0.6 : 1.0
                    }
                    Text {
                        x: 14; width: 20
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.number
                        color: Theme.textTertiary
                        horizontalAlignment: Text.AlignRight
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                    }
                    Text {
                        objectName: "memoryLocationName"
                        x: 39; width: 150
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.name
                        elide: Text.ElideRight
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                    }
                    Text {
                        x: 195; width: 90
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.start
                        elide: Text.ElideRight
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                    }
                    Repeater {
                        model: [[289, row.modelData.end], [375, row.modelData.length]]
                        Text {
                            required property var modelData
                            x: modelData[0]; width: 80
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData[1]
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                        }
                    }
                    SButton {
                        objectName: "memoryLocationRemove"
                        anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
                        text: "×"; implicitWidth: 20; implicitHeight: 18; fontPx: Theme.textSmall
                        onClicked: win.session.removeMarker(row.index)
                    }
                }
            }
        }

        Row {
            spacing: Theme.space2
            SButton {
                objectName: "memoryLocationsNew"
                text: "New…"; implicitWidth: 90; fontPx: Theme.textSmall
                onClicked: win.shell.invoke("marker.add", -1)
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "A click recalls a location: the insertion point, or the selection, moves to it. Alt-click a marker on the ruler to clear it."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
