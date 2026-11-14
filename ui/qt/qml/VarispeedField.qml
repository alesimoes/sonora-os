// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback speed in percent with the pitch shift in semitones beside it.
// Click to type (Return sends, Escape leaves), double-click resets to 100 %.
// A refused entry is named beside the field and held until the next entry.
import QtQuick
import Sonora

Item {
    id: field
    required property ShellModel shell
    property alias model: varispeed
    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    TransportVarispeed { id: varispeed; shell: field.shell }

    function close() { input.visible = false; input.focus = false }

    Row {
        id: row
        spacing: Theme.space2
        Item {
            width: 64; height: shown.implicitHeight
            Text {
                id: shown
                objectName: "varispeedText"
                visible: !input.visible
                text: varispeed.percentText
                color: varispeed.atUnity ? Theme.textSecondary : Theme.accent
                font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.Medium; features: { "tnum": 1 } }
            }
            MouseArea {
                objectName: "varispeedArea"
                anchors.fill: parent
                enabled: !input.visible
                onClicked: { input.text = String(varispeed.percent); input.visible = true; input.forceActiveFocus(); input.selectAll() }
                onDoubleClicked: { field.close(); varispeed.reset() }
            }
            TextInput {
                id: input
                objectName: "varispeedInput"
                anchors.fill: parent
                visible: false
                clip: true
                color: varispeed.refusal.length > 0 ? Theme.alarm : Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                onActiveFocusChanged: {
                    field.shell.setTextEntry(activeFocus)
                    if (!activeFocus) visible = false
                }
                Keys.onEscapePressed: field.close()
                Keys.onReturnPressed: if (varispeed.submit(text)) field.close()
                Keys.onEnterPressed: if (varispeed.submit(text)) field.close()
            }
        }
        Text {
            objectName: "varispeedSemitones"
            text: varispeed.semitonesText
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        Text {
            objectName: "varispeedRefusal"
            visible: varispeed.refusal.length > 0
            text: varispeed.refusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }
}
