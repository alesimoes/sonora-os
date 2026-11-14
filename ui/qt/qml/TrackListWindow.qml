// SPDX-License-Identifier: GPL-3.0-or-later
//
// Show/Hide Tracks (Pro Tools' Tracks list, ADR-0106): every track in order,
// whether it is on screen, and the height of its row. A hidden track is not
// gone: it plays, records and is counted as before. An armed track cannot be
// hidden, and the refusal stays at the foot of the window until it is
// dismissed. Both controls go through TrackViewModel, which is what the
// track.hide and track.height commands run.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property var views
    property Item mover: win

    width: 360
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: win.views.open

    readonly property var presets: ["small", "medium", "large", "jumbo"]

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
                text: "Tracks"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "trackListShowAll"
                anchors { right: closeButton.left; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                text: "Show All"; implicitWidth: 64; implicitHeight: 20; fontPx: Theme.textSmall
                available: win.views.hiddenCount > 0
                onClicked: win.views.showAllHeld()
            }
            SButton {
                id: closeButton
                objectName: "trackListClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.views.open = false
            }
        }

        Repeater {
            model: win.views.rows
            Rectangle {
                id: entry
                required property var modelData
                objectName: "trackListRow" + modelData.index
                width: column.width
                height: 28
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                opacity: modelData.hidden ? 0.55 : 1.0

                SButton {
                    objectName: "trackListVisible" + entry.modelData.index
                    anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    text: entry.modelData.hidden ? "Show" : "Hide"
                    implicitWidth: 44; implicitHeight: 20; fontPx: Theme.textSmall
                    onClicked: win.views.setHiddenHeld(entry.modelData.index, !entry.modelData.hidden)
                }
                Text {
                    anchors { left: parent.left; leftMargin: 56; right: presetRow.left; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    elide: Text.ElideRight
                    text: entry.modelData.name + (entry.modelData.armed ? "  ● armed" : "")
                    color: entry.modelData.armed ? Theme.record : Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Row {
                    id: presetRow
                    anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    spacing: 2
                    Repeater {
                        model: win.presets
                        SButton {
                            required property string modelData
                            objectName: "trackListHeight" + entry.modelData.index + "_" + modelData
                            text: modelData.charAt(0).toUpperCase()
                            implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textSmall
                            active: entry.modelData.height === modelData
                            onClicked: win.views.setHeightHeld(entry.modelData.index, modelData)
                        }
                    }
                }
            }
        }

        // A refusal stays until it is read.
        Rectangle {
            visible: win.views.notice !== ""
            width: parent.width
            height: noticeText.implicitHeight + 2 * Theme.space2
            radius: Theme.radiusSmall
            color: Theme.sunken
            border.color: Theme.record
            Text {
                id: noticeText
                objectName: "trackListNotice"
                anchors { left: parent.left; right: dismiss.left; verticalCenter: parent.verticalCenter; margins: Theme.space2 }
                wrapMode: Text.WordWrap
                text: win.views.notice
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            SButton {
                id: dismiss
                objectName: "trackListDismiss"
                anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                text: "OK"; implicitWidth: 30; implicitHeight: 20; fontPx: Theme.textSmall
                onClicked: win.views.dismissNotice()
            }
        }
    }
}
