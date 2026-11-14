// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data: the source session's tracks to tick, the request
// switches, and the copy's progress. A track v1 cannot import stays listed,
// dimmed, with the reason. A refusal is held in the alarm colour until
// dismissed.
import QtQuick
import Sonora

Item {
    id: panel
    required property var model
    objectName: "importSessionPanel"
    visible: false
    anchors.fill: parent
    z: 10

    function show() { visible = true }
    function close() { if (!model.busy) visible = false }

    Connections {
        target: panel.model
        function onImported(tracks) { panel.visible = false }
    }

    MouseArea { anchors.fill: parent; onClicked: panel.close() }

    Rectangle {
        anchors.centerIn: parent
        width: 640
        height: 480
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            anchors { fill: parent; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Import Session Data"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            ListView {
                objectName: "importSessionTracks"
                width: parent.width
                height: 240
                clip: true
                model: panel.model.tracks
                delegate: Text {
                    required property var modelData
                    width: ListView.view.width
                    text: (modelData.chosen ? "[x] " : "[ ] ") + modelData.name
                          + (modelData.importable ? "" : "  (" + modelData.why + ")")
                    opacity: modelData.importable ? 1.0 : 0.45
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    MouseArea { anchors.fill: parent; onClicked: panel.model.toggle(modelData.id) }
                }
            }
            Row {
                spacing: Theme.space3
                Repeater {
                    model: [["Material", "material"], ["Mix", "mix"], ["Inserts", "inserts"],
                            ["Automation", "automation"], ["Markers", "markers"]]
                    delegate: Text {
                        required property var modelData
                        text: (panel.model[modelData[1]] ? "[x] " : "[ ] ") + modelData[0]
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: panel.model[modelData[1]] = !panel.model[modelData[1]]
                        }
                    }
                }
            }
            Text {
                objectName: "importSessionNotes"
                width: parent.width
                text: panel.model.notes
                visible: text.length > 0
                wrapMode: Text.WordWrap
                color: Theme.textDim
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Text {
                objectName: "importSessionRefusal"
                width: parent.width
                text: panel.model.refusal
                visible: text.length > 0
                wrapMode: Text.WordWrap
                color: Theme.alarm
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                MouseArea { anchors.fill: parent; onClicked: panel.model.dismissRefusal() }
            }
            Row {
                spacing: Theme.space2
                SButton {
                    objectName: "importSessionGo"
                    text: panel.model.busy ? panel.model.progress + "%" : "Import"
                    enabled: !panel.model.busy && panel.model.selectedCount > 0
                    onClicked: panel.model.startImport()
                }
                SButton {
                    text: panel.model.busy ? "Cancel" : "Close"
                    onClicked: panel.model.busy ? panel.model.cancelImport() : panel.close()
                }
            }
        }
    }
}
