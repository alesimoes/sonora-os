// SPDX-License-Identifier: GPL-3.0-or-later
//
// A closed folder (ADR-0048): one narrow card where its first track was, in
// place of every track it holds. It names the folder and how many tracks it
// hides, and mutes or solos them all at once; a click on the name opens it
// again. Whether a folder is shown closed is the window's, not the
// session's.
import QtQuick
import Sonora

Rectangle {
    id: card
    required property MixModel mixer
    property var info: ({ name: "", count: 0, mute: false, solo: false, folder: 0 })

    objectName: "folderCard" + info.folder
    color: Theme.panel
    radius: Theme.radius
    border.color: Theme.borderStrong

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
        height: 3
        radius: Theme.radiusSmall
        color: Theme.textSecondary
    }

    Column {
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.space3 }
        spacing: Theme.space2
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Folder"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        Text {
            objectName: "folderCount" + card.info.folder
            anchors.horizontalCenter: parent.horizontalCenter
            text: card.info.count === 1 ? "1 track" : card.info.count + " tracks"
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        SButton {
            objectName: "folderMute" + card.info.folder
            anchors.horizontalCenter: parent.horizontalCenter
            text: "M"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
            active: card.info.mute; activeColour: Theme.mute
            onClicked: card.mixer.toggleFolderMute(card.info.folder)
        }
        SButton {
            objectName: "folderSolo" + card.info.folder
            anchors.horizontalCenter: parent.horizontalCenter
            text: "S"; implicitWidth: 18; implicitHeight: 18; fontPx: Theme.textCaption
            active: card.info.solo; activeColour: Theme.solo
            onClicked: card.mixer.toggleFolderSolo(card.info.folder)
        }
        SButton {
            objectName: "folderOpen" + card.info.folder
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Open"; implicitHeight: 18; fontPx: Theme.textCaption; bold: false
            onClicked: card.mixer.toggleFolderOpen(card.info.folder)
        }
        SButton {
            objectName: "folderDissolve" + card.info.folder
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Ungroup"; implicitHeight: 18; fontPx: Theme.textCaption; bold: false
            onClicked: card.mixer.dissolveFolder(card.info.folder)
        }
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.space1 }
        height: 22
        radius: Theme.radiusSmall
        color: Theme.raised
        Text {
            anchors { fill: parent; margins: 2 }
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: card.info.name
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
    }
}
