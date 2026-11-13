// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Live Loops grid (ADR-0164): one column per track, one row per scene,
// a launch button at the head of each row, the quantise selector in the
// header. A cell is drawn in the state the launcher reports (stopped,
// queued, playing, queued stop, armed, recording), never in the one that
// was asked for. A refusal stays at the foot until dismissed.
// Everything is LiveLoopsModel's; this draws it.
import QtQuick
import Sonora

Rectangle {
    id: panel
    property var loops: null
    readonly property int cellW: 84
    readonly property int cellH: 34

    function stateColour(s) {
        switch (s) {
        case "playing": return Theme.accent
        case "queued": return Theme.caution
        case "queuedStop": return Theme.caution
        case "armed": return Theme.record
        case "recording": return Theme.record
        default: return Theme.raised
        }
    }
    function stateLabel(s) {
        switch (s) {
        case "playing": return "Playing"
        case "queued": return "Queued"
        case "queuedStop": return "Stopping"
        case "armed": return "Armed"
        case "recording": return "Recording"
        default: return ""
        }
    }

    width: 140 + (loops !== null ? loops.trackCount : 0) * (cellW + 4) + 2 * Theme.space4
    height: 130 + (loops !== null ? loops.scenes.length : 0) * (cellH + 4) + (notice.visible ? 32 : 0)
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong

    MouseArea { anchors.fill: parent }

    Text {
        id: title
        anchors { left: parent.left; top: parent.top; margins: Theme.space4 }
        text: "Live Loops"
        color: Theme.text
        font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
    }

    Row {
        id: quant
        anchors { right: parent.right; top: parent.top; margins: Theme.space4 }
        spacing: Theme.space2
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "QUANTISE"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
        }
        Repeater {
            model: panel.loops !== null ? panel.loops.quantizeNames : []
            SButton {
                required property string modelData
                required property int index
                objectName: "llQuantize" + index
                implicitHeight: 24
                text: modelData
                active: panel.loops.quantize === index
                onClicked: panel.loops.quantize = index
            }
        }
    }

    SButton {
        objectName: "llStopAll"
        anchors { left: parent.left; top: title.bottom; margins: Theme.space4 }
        width: 108
        implicitHeight: 24
        text: "Stop all"
        onClicked: panel.loops.stopAll()
    }

    Row {
        id: heads
        anchors { left: parent.left; top: title.bottom; leftMargin: Theme.space4 + 112
                  topMargin: Theme.space4 }
        spacing: 4
        Repeater {
            model: panel.loops !== null ? panel.loops.trackCount : 0
            SButton {
                required property int index
                objectName: "llStopTrack" + index
                width: panel.cellW
                implicitHeight: 24
                bold: false
                text: "Stop " + (index + 1)
                onClicked: panel.loops.stopTrack(index)
            }
        }
    }

    Column {
        id: rows
        anchors { left: parent.left; top: heads.bottom; leftMargin: Theme.space4
                  topMargin: Theme.space3 }
        spacing: 4
        Repeater {
            model: panel.loops !== null ? panel.loops.scenes : []
            Row {
                id: srow
                required property var modelData
                spacing: 4
                SButton {
                    objectName: "llScene" + srow.modelData.scene
                    width: 108
                    height: panel.cellH
                    alignLeft: true
                    text: "▶ Scene " + (srow.modelData.scene + 1)
                    onClicked: panel.loops.triggerScene(srow.modelData.scene)
                }
                Repeater {
                    model: srow.modelData.cells
                    Rectangle {
                        id: cell
                        required property var modelData
                        objectName: "llCell" + modelData.track + "_" + modelData.scene
                        property string cellState: modelData.state
                        width: panel.cellW
                        height: panel.cellH
                        radius: Theme.radiusSmall
                        color: modelData.hasClip ? panel.stateColour(modelData.state) : Theme.sunken
                        opacity: modelData.hasClip ? 1.0 : 0.6
                        border.color: modelData.hasClip ? Theme.borderStrong : Theme.borderSubtle
                        Text {
                            anchors.centerIn: parent
                            text: !cell.modelData.hasClip ? "empty" : panel.stateLabel(cell.modelData.state)
                            color: cell.modelData.state === "stopped" || !cell.modelData.hasClip
                                   ? Theme.textSecondary : Theme.textOnAccent
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: panel.loops.triggerCell(cell.modelData.track, cell.modelData.scene)
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: notice
        objectName: "llNotice"
        visible: panel.loops !== null && panel.loops.notice.length > 0
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.space4 }
        height: 24
        radius: Theme.radius
        color: Theme.sunken
        border.color: Theme.caution
        Text {
            anchors { fill: parent; leftMargin: Theme.space2; rightMargin: 60 }
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: panel.loops !== null ? panel.loops.notice : ""
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        SButton {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 2 }
            implicitHeight: 20
            text: "Dismiss"
            onClicked: panel.loops.dismissNotice()
        }
    }
}
