// SPDX-License-Identifier: GPL-3.0-or-later
//
// The automation Event List: Logic's Event List window for the breakpoints
// of one lane of the selected track, floating over the Edit or Mix window.
// Every point in time order; the time and the value are typed in place (a
// click puts the field open, Return sets it), the row's x deletes the point,
// and Snapshot adds one at the insertion point holding the strip's fader or
// pan. Each change is one undo step, through the same EditAutomation as the
// timeline's own gestures.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    // What a drag on the title moves: the item holding the window.
    property Item mover: win
    readonly property int rowHeight: 24
    readonly property int shownRows: 9

    width: 440
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.eventListOpen

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
                text: "Automation Event List"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "eventListClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.shell.invoke("window.event_list", -1)
            }
        }

        Row {
            spacing: Theme.space1
            Repeater {
                model: win.session.autolaneNames
                SButton {
                    required property string modelData
                    required property int index
                    objectName: "eventListLane" + index
                    text: modelData; implicitWidth: 92; fontPx: Theme.textSmall
                    active: win.session.eventLane === index
                    onClicked: win.session.setEventLane(index)
                }
            }
        }

        Text {
            objectName: "eventListTitle"
            width: parent.width
            elide: Text.ElideRight
            text: win.session.eventListTitle
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }

        Item {
            width: parent.width
            height: 14
            Repeater {
                model: [["#", 26], ["TIME", 112], ["VALUE", 252]]
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
                text: "No breakpoints on this lane. Snapshot adds one at the insertion point, or draw on the lane with the Pencil."
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }

            ListView {
                id: list
                objectName: "eventListRows"
                anchors { fill: parent; margins: 1 }
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: win.session.eventList
                delegate: Rectangle {
                    id: row
                    objectName: "eventListRow"
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: win.rowHeight
                    color: "transparent"
                    Text {
                        x: 2; width: 36
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.number
                        color: Theme.textTertiary
                        horizontalAlignment: Text.AlignRight
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                    }
                    // Each field closes at once and the edit runs after the key
                    // handler returns: a good edit rebuilds the rows, this one
                    // with them, and must not do it from inside its own handler.
                    ValueField {
                        id: timeField
                        objectName: "eventTime"
                        shell: win.shell
                        x: 56; width: 150; height: parent.height
                        text: row.modelData.time
                        textColour: Theme.textSecondary
                        onSubmitted: (t) => {
                            const at = row.index
                            timeField.finish()
                            Qt.callLater(() => { if (!win.session.eventEdit(at, t, "")) timeField.begin() })
                        }
                    }
                    ValueField {
                        id: valueField
                        objectName: "eventValue"
                        shell: win.shell
                        x: 216; width: 110; height: parent.height
                        text: row.modelData.value
                        onSubmitted: (t) => {
                            const at = row.index
                            valueField.finish()
                            Qt.callLater(() => { if (!win.session.eventEdit(at, "", t)) valueField.begin() })
                        }
                    }
                    Text {
                        x: 330
                        anchors.verticalCenter: parent.verticalCenter
                        visible: row.modelData.step
                        text: "step"
                        color: Theme.textTertiary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                    }
                    SButton {
                        objectName: "eventDelete"
                        anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
                        text: "×"; implicitWidth: 20; implicitHeight: 18; fontPx: Theme.textSmall
                        onClicked: win.session.eventDelete(row.index)
                    }
                }
            }
        }

        Row {
            spacing: Theme.space2
            SButton {
                objectName: "eventListSnapshot"
                text: "Snapshot"; implicitWidth: 90; fontPx: Theme.textSmall
                onClicked: win.shell.invoke("automation.snapshot", -1)
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Click a time or value to type a new one; a point stays between its neighbours. Snapshot holds the fader or pan as it stands at the insertion point."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
