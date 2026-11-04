// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window > Configuration (Pro Tools) / Screensets (Logic): nine numbered
// slots, each holding which of Edit and Mix is in front and which floating
// windows are open and where. Store puts the screen as it is into the slot;
// Recall puts the screen as the slot had it. An empty slot refuses to
// recall, and the refusal stays here until the next action: the screen must
// not move on a guess. Both go through window.config_store and
// window.config_recall, as a key bound to them would.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    property Item mover: win

    width: 380
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.windowConfigsOpen

    // The refusal of the last action, cleared by the next one.
    property string refusal: ""
    function run(command, slot) {
        win.refusal = ""
        if (!win.shell.invokeWith(command, -1, String(slot))) win.refusal = win.shell.statusText
    }

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
                text: "Window Configurations"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "windowConfigsClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.session.setWindowConfigsOpen(false)
            }
        }

        Repeater {
            model: win.session.windowConfigs
            Rectangle {
                required property var modelData
                objectName: "windowConfigRow" + modelData.slot
                width: column.width
                height: 28
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                Text {
                    anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    text: modelData.slot
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                }
                Text {
                    objectName: "windowConfigSummary" + modelData.slot
                    anchors { left: parent.left; leftMargin: 32; verticalCenter: parent.verticalCenter }
                    text: modelData.summary
                    color: modelData.stored ? Theme.text : Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Row {
                    anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
                    spacing: Theme.space1
                    SButton {
                        objectName: "windowConfigStore" + modelData.slot
                        text: "Store"; implicitHeight: 22
                        onClicked: win.run("window.config_store", modelData.slot)
                    }
                    SButton {
                        objectName: "windowConfigRecall" + modelData.slot
                        text: "Recall"; implicitHeight: 22
                        available: modelData.stored
                        onClicked: win.run("window.config_recall", modelData.slot)
                    }
                }
            }
        }

        Text {
            objectName: "windowConfigRefusal"
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: win.refusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Kept for this machine, not in the session. A window that does not fit the screen is moved back inside it."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
