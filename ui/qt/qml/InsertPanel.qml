// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generic parameter panel of one insert, built from the engine's
// catalog (ADR-0012 §8): a slider for a continuous parameter, a row of
// choices for one with a few steps (an enum's labels, a toggle's Off/On),
// the value in the plugin's unit beside it. Dragging a slider sends
// PluginParameterChange while it moves and records the end of the gesture
// in the session; Alt-click returns a parameter to its default.
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
        width: Math.min(420, parent.width - 2 * Theme.space6)
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
                Repeater {
                    model: panelRoot.inserts !== null ? panelRoot.inserts.panelParams : []
                    Item {
                        id: paramRow
                        required property var modelData
                        objectName: "param_" + modelData.key
                        readonly property bool choices: modelData.steps >= 2 && modelData.steps <= 6
                        readonly property var now: panelRoot.inserts !== null
                                                   && panelRoot.inserts.panelValues[modelData.key] !== undefined
                                                   ? panelRoot.inserts.panelValues[modelData.key]
                                                   : ({ text: "", position: 0, step: -1 })
                        width: params.width
                        height: 28

                        Text {
                            id: paramName
                            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                            width: 104
                            elide: Text.ElideRight
                            text: paramRow.modelData.name
                            color: Theme.textSecondary
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                        }

                        // A few steps: one button per choice.
                        Row {
                            visible: paramRow.choices
                            anchors { left: paramName.right; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                            spacing: 2
                            Repeater {
                                model: paramRow.choices ? paramRow.modelData.labels : []
                                SButton {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    implicitHeight: 22
                                    fontPx: Theme.textCaption
                                    active: paramRow.now.step === index
                                    onClicked: panelRoot.inserts.paramStep(paramRow.modelData.key, index)
                                }
                            }
                        }

                        // Otherwise a slider.
                        Rectangle {
                            id: sliderTrack
                            visible: !paramRow.choices
                            anchors { left: paramName.right; right: valueText.left; leftMargin: Theme.space2
                                      rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                            height: 6
                            radius: Theme.radiusSmall
                            color: Theme.sunken
                            border.color: Theme.borderSubtle
                            Rectangle {
                                width: Math.max(2, paramRow.now.position * parent.width)
                                height: parent.height
                                radius: Theme.radiusSmall
                                color: Theme.accent
                            }
                            Rectangle {
                                x: paramRow.now.position * (parent.width - width)
                                anchors.verticalCenter: parent.verticalCenter
                                width: 10; height: 16
                                radius: Theme.radiusSmall
                                color: Theme.hover
                                border.color: Theme.borderStrong
                            }
                            MouseArea {
                                anchors { fill: parent; topMargin: -8; bottomMargin: -8 }
                                function at(x) { return Math.max(0, Math.min(1, x / width)) }
                                onPressed: (m) => {
                                    if (m.modifiers & Qt.AltModifier) {
                                        panelRoot.inserts.paramReset(paramRow.modelData.key)
                                        return
                                    }
                                    panelRoot.inserts.paramBegin()
                                    panelRoot.inserts.paramPosition(paramRow.modelData.key, at(m.x))
                                }
                                onPositionChanged: (m) => {
                                    if (pressed && !(m.modifiers & Qt.AltModifier))
                                        panelRoot.inserts.paramPosition(paramRow.modelData.key, at(m.x))
                                }
                                onReleased: panelRoot.inserts.paramEnd()
                                onCanceled: panelRoot.inserts.paramEnd()
                            }
                        }
                        Text {
                            id: valueText
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            width: paramRow.choices ? 0 : 76
                            visible: !paramRow.choices
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                            text: paramRow.now.text
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                        }
                    }
                }
            }
        }
    }
}
