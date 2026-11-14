// SPDX-License-Identifier: GPL-3.0-or-later
//
// The editor of one insert, built from the engine's catalog alone
// (ADR-0011 §2.6, ADR-0012 §8, ADR-0057), the same for every plugin: the
// controls grouped in the sections the plugin declares, each drawn as the
// plugin asks (a knob, a fader, a switch, a row of choices) or, when it
// says nothing, a knob for a continuous parameter, a switch for a toggle, a
// row of choices for a few labelled steps. Values show with their unit.
// Dragging a control sends PluginParameterChange while it moves and records
// the end of the gesture in the session; Alt-click returns it to its
// default.
//
// What the plugin publishes is drawn above the controls: the curves
// (an EQ's response, a compressor's transfer curve) and the meters (gain
// reduction, input and output level), from the telemetry the engine
// forwards. A meter or curve with nothing behind it says so rather than
// drawing zero, which would read as silence. The header carries bypass and
// A/B compare: B starts as a copy of A, and each toggle plays the other set
// as one undoable gesture. Presets are not here: the engine's preset store
// has no interface message yet.
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
                // A/B compare: the lit state is the second set.
                SButton {
                    objectName: "insertPanelCompare"
                    text: "A/B  " + (panelRoot.inserts !== null ? panelRoot.inserts.compareSlot : "A")
                    implicitHeight: 22
                    active: panelRoot.inserts !== null && panelRoot.inserts.compareSlot === "B"
                    onClicked: panelRoot.inserts.toggleCompare()
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

        // What the plugin publishes, above the controls and not scrolled
        // away with them: its curves, then its meters.
        Column {
            id: telemetry
            objectName: "insertPanelTelemetry"
            anchors { left: parent.left; right: parent.right; top: rule.bottom
                      leftMargin: Theme.space4; rightMargin: Theme.space4; topMargin: Theme.space3 }
            spacing: Theme.space1
            readonly property var meters: panelRoot.inserts !== null ? panelRoot.inserts.panelMeters : []
            readonly property var curves: panelRoot.inserts !== null ? panelRoot.inserts.panelCurves : []
            readonly property string tstate: panelRoot.inserts !== null ? panelRoot.inserts.telemetryState : "none"
            visible: meters.length > 0 || curves.length > 0
            height: visible ? implicitHeight : 0

            Repeater {
                model: telemetry.curves
                CurveView {
                    required property var modelData
                    objectName: "curve_" + modelData.id
                    width: telemetry.width
                    title: modelData.name
                    xUnit: modelData.xUnit
                    yUnit: modelData.yUnit
                    xLog: modelData.xLog
                    xMin: modelData.xMin
                    xMax: modelData.xMax
                    yMin: modelData.yMin
                    yMax: modelData.yMax
                    emptyText: telemetry.tstate === "stopped" ? "the plugin stopped publishing" : "waiting for the plugin"
                    points: panelRoot.inserts.panelCurveData[modelData.id] !== undefined
                            ? panelRoot.inserts.panelCurveData[modelData.id] : []
                }
            }
            Repeater {
                model: telemetry.meters
                MeterBar {
                    required property var modelData
                    objectName: "meter_" + modelData.id
                    width: telemetry.width
                    label: modelData.kind === "gain_reduction" ? "GR" : modelData.kind === "input" ? "IN"
                           : modelData.kind === "output" ? "OUT" : modelData.name
                    kind: modelData.kind
                    readonly property var now: panelRoot.inserts.panelMeterValues[modelData.id] !== undefined
                                               ? panelRoot.inserts.panelMeterValues[modelData.id] : ({ valid: false })
                    valid: now.valid === true
                    fraction: now.valid === true ? now.fraction : 0
                    valueText: now.valid === true ? now.text : ""
                }
            }
            Text {
                visible: telemetry.tstate === "stopped" || telemetry.tstate === "waiting"
                width: parent.width
                wrapMode: Text.Wrap
                text: telemetry.tstate === "stopped" ? "Meter data stopped: the plugin is no longer publishing."
                                                    : "Waiting for the plugin's meter data."
                color: telemetry.tstate === "stopped" ? Theme.caution : Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
        }

        Flickable {
            id: flick
            anchors { left: parent.left; right: parent.right; top: telemetry.bottom; bottom: parent.bottom
                      margins: Theme.space4; topMargin: Theme.space3 }
            clip: true
            contentHeight: params.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: params
                width: flick.width
                spacing: Theme.space2
                // As many knob cells as fit, sharing the width evenly.
                readonly property int columns: Math.max(1, Math.floor(width / 84))
                readonly property real cell: Math.floor(width / columns)
                Text {
                    visible: text.length > 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: panelRoot.inserts !== null ? panelRoot.inserts.panelNote : ""
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Repeater {
                    model: panelRoot.inserts !== null ? panelRoot.inserts.panelSections : []
                    Column {
                        id: section
                        required property var modelData
                        objectName: "section_" + modelData.name
                        width: params.width
                        spacing: 0
                        Text {
                            visible: section.modelData.name.length > 0
                            width: parent.width
                            height: visible ? implicitHeight + Theme.space1 : 0
                            text: section.modelData.name.toUpperCase()
                            color: Theme.textSecondary
                            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.Bold; letterSpacing: 0.6 }
                        }
                        Rectangle {
                            visible: section.modelData.name.length > 0
                            width: parent.width
                            height: visible ? 1 : 0
                            color: Theme.borderSubtle
                        }
                        Flow {
                            id: grid
                            width: params.width
                            spacing: 0
                            Repeater {
                                model: section.modelData.params
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
                                    width: row ? grid.width : params.cell
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
                                        laneActive: paramCell.modelData.automated === true
                                        onBegin: panelRoot.inserts.paramBegin()
                                        onMoved: (p) => panelRoot.inserts.paramPosition(paramCell.modelData.key, p)
                                        onEnd: panelRoot.inserts.paramEnd()
                                        onReset: panelRoot.inserts.paramReset(paramCell.modelData.key)
                                    }

                                    Fader {
                                        objectName: "fader_" + paramCell.modelData.key
                                        visible: paramCell.control === "fader"
                                        anchors { top: cellName.bottom; topMargin: 0; horizontalCenter: parent.horizontalCenter }
                                        travel: 56
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

                                    // The value, with its unit, under a knob or a fader.
                                    Text {
                                        visible: paramCell.control === "knob" || paramCell.control === "fader"
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
                                    Flow {
                                        visible: paramCell.row
                                        anchors { left: rowName.right; leftMargin: Theme.space2; right: parent.right
                                                  verticalCenter: parent.verticalCenter }
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
    }
}
