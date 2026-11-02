// SPDX-License-Identifier: GPL-3.0-or-later
//
// The editor of one insert, built from the engine's catalog alone
// (ADR-0011 §2.6, ADR-0012 §8), the same for every plugin: a knob for a
// continuous parameter (and for one with too many steps to show at once),
// a switch for a toggle, a row of choices for a few labelled steps, in the
// order the plugin lists them. Dragging a knob sends PluginParameterChange
// while it moves and records the end of the gesture in the session;
// Alt-click returns any control to its default.
//
// No meters: the read-only telemetry region a plugin's gain reduction
// would come through is not built yet, and a meter with nothing behind it
// would read as silence.
//
// A faulted insert shows the engine's reason at the top with what can be
// done about it: retry the chain, take the insert out of it, or remove it.
import QtQuick
import Sonora

Item {
    id: panelRoot
    required property ShellModel shell
    property InsertModel inserts: null
    readonly property var info: inserts !== null ? inserts.panelSlotInfo : ({})

    // Behind the panel: a click closes it, the mixer stays visible.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: panelRoot.inserts.closePanel()
    }

    Rectangle {
        id: panel
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom; margins: Theme.space3 }
        width: Math.min(456, parent.width - 2 * Theme.space6)
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click inside does not close it

        // A soft edge toward the mixer.
        Rectangle {
            anchors { right: parent.left; top: parent.top; bottom: parent.bottom }
            width: 3
            color: Theme.shadow
        }

        Column {
            id: head
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space1
            Item {
                width: parent.width
                height: titleText.implicitHeight
                Text {
                    id: titleText
                    objectName: "insertPanelTitle"
                    anchors { left: parent.left; right: doneButton.left; rightMargin: Theme.space2 }
                    elide: Text.ElideRight
                    text: panelRoot.inserts !== null ? panelRoot.inserts.panelTitle : ""
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
                }
                SButton {
                    id: doneButton
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    text: "Done"
                    implicitHeight: 22
                    onClicked: panelRoot.inserts.closePanel()
                }
            }
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: panelRoot.inserts !== null ? panelRoot.inserts.panelSubtitle : ""
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                spacing: Theme.space1
                topPadding: Theme.space1
                SButton {
                    objectName: "insertPanelBypass"
                    text: "Bypass"
                    implicitHeight: 22
                    active: panelRoot.info.bypass === true
                    activeColour: Theme.bypass
                    onClicked: panelRoot.inserts.toggleBypass(panelRoot.inserts.panelTrack, panelRoot.inserts.panelSlot)
                }
                SButton {
                    text: panelRoot.info.active === false ? "Make active" : "Make inactive"
                    implicitHeight: 22
                    onClicked: panelRoot.inserts.setActive(panelRoot.inserts.panelTrack, panelRoot.inserts.panelSlot,
                                                           panelRoot.info.active === false)
                }
                SButton {
                    text: "Remove"
                    implicitHeight: 22
                    onClicked: panelRoot.inserts.remove(panelRoot.inserts.panelTrack, panelRoot.inserts.panelSlot)
                }
            }

            // The fault, loud, with what to do about it.
            Rectangle {
                objectName: "insertPanelFault"
                visible: panelRoot.info.faulted === true
                width: parent.width
                height: visible ? faultColumn.implicitHeight + 2 * Theme.space2 : 0
                radius: Theme.radius
                color: Theme.alarmSurface
                border.color: Theme.alarm
                Column {
                    id: faultColumn
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space2 }
                    spacing: Theme.space1
                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: panelRoot.info.missing === true ? "NOT INSTALLED" : "NOT RUNNING"
                        color: Theme.alarm
                        font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.Bold; letterSpacing: 0.6 }
                    }
                    Text {
                        objectName: "insertPanelFaultText"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: panelRoot.info.fault !== undefined ? panelRoot.info.fault : ""
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                    }
                    Row {
                        spacing: Theme.space1
                        SButton {
                            text: "Retry"
                            implicitHeight: 22
                            available: panelRoot.shell.implemented("plugin.retry")
                            onClicked: panelRoot.shell.invoke("plugin.retry", panelRoot.inserts.panelTrack)
                        }
                        SButton {
                            text: "Make inactive"
                            implicitHeight: 22
                            onClicked: panelRoot.inserts.setActive(panelRoot.inserts.panelTrack, panelRoot.inserts.panelSlot, false)
                        }
                    }
                }
            }
        }

        Rectangle {
            id: rule
            anchors { left: parent.left; right: parent.right; top: head.bottom; topMargin: Theme.space3 }
            height: 1
            color: Theme.borderSubtle
        }

        Flickable {
            id: flick
            anchors { left: parent.left; right: parent.right; top: rule.bottom; bottom: parent.bottom
                      margins: Theme.space4 }
            clip: true
            contentHeight: params.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: params
                width: flick.width
                spacing: Theme.space2
                Text {
                    visible: text.length > 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: panelRoot.inserts !== null ? panelRoot.inserts.panelNote : ""
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Flow {
                    id: grid
                    width: params.width
                    spacing: 0
                    // As many knob cells as fit, sharing the width evenly.
                    readonly property int columns: Math.max(1, Math.floor(width / 84))
                    readonly property real cell: Math.floor(width / columns)
                    Repeater {
                        model: panelRoot.inserts !== null ? panelRoot.inserts.panelParams : []
                        Item {
                            id: paramCell
                            required property var modelData
                            objectName: "param_" + modelData.key
                            readonly property string control: modelData.control
                            readonly property bool row: control === "choices"
                            readonly property var now: panelRoot.inserts !== null
                                                       && panelRoot.inserts.panelValues[modelData.key] !== undefined
                                                       ? panelRoot.inserts.panelValues[modelData.key]
                                                       : ({ text: "", position: 0, step: -1 })
                            width: row ? grid.width : grid.cell
                            height: row ? 36 : 108

                            Text {
                                id: cellName
                                visible: !paramCell.row
                                anchors { top: parent.top; topMargin: Theme.space1; horizontalCenter: parent.horizontalCenter }
                                width: parent.width - Theme.space1
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: paramCell.modelData.name
                                color: Theme.textSecondary
                                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                            }

                            Knob {
                                objectName: "knob_" + paramCell.modelData.key
                                visible: paramCell.control === "knob"
                                anchors { top: cellName.bottom; topMargin: 6; horizontalCenter: parent.horizontalCenter }
                                position: paramCell.now.position
                                origin: paramCell.modelData.origin
                                defaultPosition: paramCell.modelData.defaultPosition
                                onBegin: panelRoot.inserts.paramBegin()
                                onMoved: (p) => panelRoot.inserts.paramPosition(paramCell.modelData.key, p)
                                onEnd: panelRoot.inserts.paramEnd()
                                onReset: panelRoot.inserts.paramReset(paramCell.modelData.key)
                            }

                            // A switch: lit when on, a click flips it.
                            SButton {
                                objectName: "toggle_" + paramCell.modelData.key
                                visible: paramCell.control === "toggle"
                                anchors { verticalCenter: parent.verticalCenter; verticalCenterOffset: 4
                                          horizontalCenter: parent.horizontalCenter }
                                implicitWidth: Math.max(52, implicitHeight)
                                implicitHeight: 26
                                text: paramCell.now.text
                                active: paramCell.now.step === 1
                                onClicked: (m) => {
                                    if (m.modifiers & Qt.AltModifier)
                                        panelRoot.inserts.paramReset(paramCell.modelData.key)
                                    else
                                        panelRoot.inserts.paramStep(paramCell.modelData.key, paramCell.now.step === 1 ? 0 : 1)
                                }
                            }

                            Text {
                                visible: paramCell.control === "knob"
                                anchors { bottom: parent.bottom; bottomMargin: Theme.space1; horizontalCenter: parent.horizontalCenter }
                                width: parent.width - Theme.space1
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: paramCell.now.text
                                color: Theme.text
                                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                            }

                            // A few choices: all of them in view, one lit.
                            Text {
                                id: rowName
                                visible: paramCell.row
                                anchors { left: parent.left; leftMargin: Theme.space1; verticalCenter: parent.verticalCenter }
                                width: 104
                                elide: Text.ElideRight
                                text: paramCell.modelData.name
                                color: Theme.textSecondary
                                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                            }
                            Row {
                                visible: paramCell.row
                                anchors { left: rowName.right; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                                spacing: 2
                                Repeater {
                                    model: paramCell.row ? paramCell.modelData.labels : []
                                    SButton {
                                        required property int index
                                        required property string modelData
                                        objectName: "choice_" + paramCell.modelData.key + "_" + index
                                        text: modelData
                                        implicitHeight: 24
                                        fontPx: Theme.textCaption
                                        active: paramCell.now.step === index
                                        onClicked: (m) => {
                                            if (m.modifiers & Qt.AltModifier)
                                                panelRoot.inserts.paramReset(paramCell.modelData.key)
                                            else
                                                panelRoot.inserts.paramStep(paramCell.modelData.key, index)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
