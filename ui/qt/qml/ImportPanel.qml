// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Audio: Pro Tools' File > Import > Audio as a browser the appliance
// can draw for itself (there is no desktop to borrow a file dialog from).
// Places on the left (the session's own audio, home, the sessions folder,
// removable media), the folder's WAV files on the right with what each is
// and whether it will be converted. Chosen files become new tracks, one
// undo step. A file that cannot be imported stays in the list, dimmed, with
// the reason: it is not hidden.
//
// A refusal stays in the panel, in the alarm colour, until it is dismissed.
import QtQuick
import Sonora

Item {
    id: panel
    required property var shell
    required property var session
    readonly property var model: session.importer()
    objectName: "importPanel"
    visible: false
    anchors.fill: parent
    z: 10

    function show() {
        model.show()
        visible = true
    }
    function close() {
        if (model.busy) return
        visible = false
    }

    Connections {
        target: panel.model
        function onImported(tracks) {
            panel.visible = false
            panel.shell.say(tracks === 1 ? "Imported 1 file as a new track" : "Imported " + tracks + " files as new tracks")
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: panel.close()
    }

    Rectangle {
        id: box
        anchors.centerIn: parent
        width: 760
        height: 520
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            anchors { fill: parent; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Import Audio"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Row {
                width: parent.width
                spacing: Theme.space2
                SButton {
                    objectName: "importUp"
                    text: "Up"
                    implicitHeight: 24
                    available: !panel.model.busy
                    onClicked: panel.model.up()
                }
                Text {
                    width: parent.width - 60
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideMiddle
                    text: panel.model.folder
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }

            Row {
                width: parent.width
                height: parent.height - 150
                spacing: Theme.space3

                Column {
                    width: 160
                    spacing: 2
                    Repeater {
                        model: panel.model.places
                        SButton {
                            required property var modelData
                            objectName: "importPlace"
                            width: 160
                            alignLeft: true
                            bold: false
                            text: modelData.name
                            available: !panel.model.busy
                            onClicked: panel.model.open(modelData.path)
                        }
                    }
                }

                Rectangle {
                    width: parent.width - 160 - Theme.space3
                    height: parent.height
                    radius: Theme.radiusSmall
                    color: Theme.sunken
                    clip: true
                    ListView {
                        id: list
                        objectName: "importList"
                        anchors { fill: parent; margins: 2 }
                        model: panel.model.entries
                        spacing: 2
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: Rectangle {
                            id: row
                            required property var modelData
                            objectName: "importRow"
                            width: list.width
                            height: 40
                            radius: Theme.radiusSmall
                            color: modelData.chosen ? Theme.selectionFill
                                   : area.containsMouse && (modelData.usable || modelData.dir) ? Theme.hover : "transparent"
                            border.color: modelData.chosen ? Theme.accent : "transparent"
                            MouseArea {
                                id: area
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: !panel.model.busy
                                onClicked: {
                                    if (row.modelData.dir) panel.model.open(row.modelData.path)
                                    else if (row.modelData.usable) panel.model.toggle(row.modelData.path)
                                }
                            }
                            Column {
                                anchors { left: parent.left; leftMargin: Theme.space3; right: parent.right
                                          rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                                spacing: 1
                                Text {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: (row.modelData.dir ? "> " : "") + row.modelData.name
                                    color: row.modelData.usable || row.modelData.dir ? Theme.text : Theme.textTertiary
                                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                                }
                                Text {
                                    visible: !row.modelData.dir
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: row.modelData.detail
                                    color: row.modelData.usable ? Theme.textSecondary : Theme.caution
                                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                                }
                            }
                        }
                    }
                    Text {
                        visible: panel.model.entries.length === 0
                        anchors.centerIn: parent
                        text: "No folders or audio files here."
                        color: Theme.textTertiary
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    }
                }
            }

            Rectangle {
                visible: panel.model.refusal.length > 0
                objectName: "importRefusal"
                width: parent.width
                height: refusalText.implicitHeight + 2 * Theme.space2
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.alarm
                Text {
                    id: refusalText
                    anchors { left: parent.left; right: dismiss.left; verticalCenter: parent.verticalCenter
                              margins: Theme.space2 }
                    wrapMode: Text.Wrap
                    text: panel.model.refusal
                    color: Theme.alarm
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                SButton {
                    id: dismiss
                    objectName: "importDismiss"
                    anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    text: "Dismiss"
                    onClicked: panel.model.dismissRefusal()
                }
            }

            Rectangle {
                visible: panel.model.busy
                objectName: "importProgress"
                width: parent.width
                height: 18
                radius: Theme.radiusSmall
                color: Theme.sunken
                Rectangle {
                    width: parent.width * panel.model.progress / 1000
                    height: parent.height
                    radius: Theme.radiusSmall
                    color: Theme.accent
                }
                Text {
                    anchors.centerIn: parent
                    text: panel.model.status
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }

            Row {
                width: parent.width
                spacing: Theme.space2
                SButton {
                    objectName: "importAtCursor"
                    text: panel.model.atInsertion ? "At the edit cursor" : "At the session start"
                    available: !panel.model.busy
                    onClicked: panel.model.atInsertion = !panel.model.atInsertion
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 440
                    elide: Text.ElideRight
                    text: panel.model.summary
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                SButton {
                    objectName: "importCancel"
                    text: panel.model.busy ? "Cancel import" : "Close"
                    onClicked: panel.model.busy ? panel.model.cancelImport() : panel.close()
                }
                SButton {
                    objectName: "importGo"
                    text: "Import"
                    active: panel.model.selectedCount > 0 && !panel.model.busy
                    available: panel.model.selectedCount > 0 && !panel.model.busy
                    onClicked: panel.model.startImport()
                }
            }
        }
    }
}
