// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smart Controls (ADR-0117): eight macro knobs on the left, the selected
// macro's mappings on the right. A knob fans out to every parameter mapped
// to it; Learn arms the selected macro and the next parameter touched in a
// plug-in panel becomes a mapping. Each mapping has its range (the two
// handles, along the parameter's own travel) and an invert switch. A
// mapping whose insert or parameter is not in the session is listed in the
// caution colour and counted on its knob, never hidden.
//
// The panel keeps no state: everything it draws is the model's, and every
// gesture is a call on it (SmartControlsModel).
import QtQuick
import Sonora

Rectangle {
    id: panel
    required property var smart
    implicitWidth: 1180
    implicitHeight: 330
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong

    readonly property var macro: smart.macros[smart.selected]

    // --- the header and its held notice ---
    Row {
        id: header
        x: Theme.space4; y: Theme.space3
        spacing: Theme.space3
        Text {
            text: "SMART CONTROLS"
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1.2 }
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: smart.learning >= 0
                  ? "Learning " + smart.macros[smart.learning].name + ": move a parameter in any plug-in panel"
                  : "Drag a knob; select it to see what it drives"
            color: smart.learning >= 0 ? Theme.record : Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Rectangle {
        id: notice
        objectName: "smartNotice"
        visible: smart.notice !== ""
        anchors { left: parent.left; right: parent.right; top: header.bottom; margins: Theme.space3 }
        height: visible ? 30 : 0
        radius: Theme.radiusSmall
        color: Theme.alarmSurface
        border.color: Theme.alarm
        Text {
            anchors { left: parent.left; right: dismiss.left; leftMargin: Theme.space3; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
            text: smart.notice
            elide: Text.ElideRight
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        SButton {
            id: dismiss
            anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
            text: "Dismiss"
            implicitHeight: 22
            onClicked: smart.dismissNotice()
        }
    }

    // --- the eight knobs, two rows of four ---
    Grid {
        id: knobs
        anchors { left: parent.left; top: notice.bottom; leftMargin: Theme.space4; topMargin: Theme.space3 }
        columns: 4
        spacing: Theme.space2
        Repeater {
            model: smart.macros
            Rectangle {
                id: cell
                required property int index
                required property var modelData
                readonly property bool chosen: index === smart.selected
                readonly property bool armed: index === smart.learning
                objectName: "macro" + index
                width: 112; height: 126
                radius: Theme.radius
                color: chosen ? Theme.raised : Theme.sunken
                border.color: armed ? Theme.record : chosen ? Theme.accent : Theme.borderSubtle
                border.width: armed || chosen ? 2 : 1

                MouseArea { anchors.fill: parent; onClicked: smart.select(cell.index) }

                Knob {
                    anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: Theme.space1 }
                    diameter: 50
                    position: cell.modelData.value
                    opacity: cell.modelData.mappings > 0 ? 1.0 : 0.5
                    onBegin: smart.select(cell.index)
                    onMoved: (p) => smart.setMacroValue(cell.index, p)
                }
                Text {
                    id: name
                    anchors { left: parent.left; right: parent.right; bottom: count.top; margins: Theme.space1 }
                    horizontalAlignment: Text.AlignHCenter
                    text: cell.modelData.name
                    elide: Text.ElideRight
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
                }
                Text {
                    id: count
                    anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: Theme.space1 }
                    text: cell.modelData.mappings === 0 ? "unmapped"
                          : cell.modelData.unresolved > 0
                            ? cell.modelData.mappings + " · " + cell.modelData.unresolved + " missing"
                            : cell.modelData.mappings + (cell.modelData.mappings === 1 ? " parameter" : " parameters")
                    color: cell.modelData.unresolved > 0 ? Theme.caution : Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
            }
        }
    }

    // --- the selected macro's mappings ---
    Rectangle {
        id: list
        anchors { left: knobs.right; right: parent.right; top: notice.bottom; bottom: parent.bottom
                  leftMargin: Theme.space4; rightMargin: Theme.space4; topMargin: Theme.space3; bottomMargin: Theme.space3 }
        radius: Theme.radius
        color: Theme.sunken
        border.color: Theme.borderSubtle

        Row {
            id: listHeader
            x: Theme.space3; y: Theme.space2
            spacing: Theme.space2
            Rectangle {
                width: 180; height: 26
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: nameInput.activeFocus ? Theme.accent : Theme.borderSubtle
                TextInput {
                    id: nameInput
                    objectName: "macroName"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    verticalAlignment: TextInput.AlignVCenter
                    text: panel.macro ? panel.macro.name : ""
                    color: Theme.text
                    clip: true
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                    onEditingFinished: smart.renameMacro(smart.selected, text)
                }
            }
            SButton {
                objectName: "learnButton"
                text: smart.learning === smart.selected ? "Done" : "Learn"
                active: smart.learning === smart.selected
                activeColour: Theme.record
                implicitHeight: 26
                onClicked: smart.learn(smart.selected)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: smart.mappings.length === 0 ? "No parameters yet: press Learn, then move one"
                                                  : smart.mappings.length + (smart.mappings.length === 1 ? " parameter" : " parameters")
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
        }

        Column {
            anchors { left: parent.left; right: parent.right; top: listHeader.bottom; margins: Theme.space3 }
            spacing: 2
            Repeater {
                model: smart.mappings
                MappingRow {}
            }
        }
    }

    component MappingRow: Rectangle {
        id: row
        required property int index
        required property var modelData
        readonly property bool missing: modelData.unresolved
        width: parent ? parent.width : 0
        height: 34
        radius: Theme.radiusSmall
        color: modelData.capturing ? Qt.darker(Theme.record, 3.2) : Theme.panel
        border.color: modelData.capturing ? Theme.record : Theme.borderSubtle

        Column {
            id: names
            anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
            width: 250
            Text {
                width: parent.width
                text: row.modelData.plugin + " · " + row.modelData.param
                elide: Text.ElideRight
                color: row.missing ? Theme.caution : Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                text: row.missing ? "unresolved: skipped when the knob moves" : row.modelData.label
                elide: Text.ElideRight
                color: row.missing ? Theme.caution : Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
        }

        Text {
            id: loText
            anchors { left: names.right; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
            width: 64
            horizontalAlignment: Text.AlignRight
            text: row.modelData.loText
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
        }

        // The range: the parameter's travel, the mapped span lit, a handle at
        // each end. Dragged locally, sent on release.
        Item {
            id: range
            anchors { left: loText.right; right: hiText.left; margins: Theme.space2; verticalCenter: parent.verticalCenter }
            height: 18
            enabled: !row.missing
            opacity: row.missing ? 0.4 : 1.0
            property real lo: row.modelData.loPos
            property real hi: row.modelData.hiPos
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width; height: 4; radius: 2
                color: Theme.borderStrong
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: Math.min(range.lo, range.hi) * range.width
                width: Math.abs(range.hi - range.lo) * range.width
                height: 4; radius: 2
                color: row.modelData.invert ? Theme.caution : Theme.accent
            }
            Repeater {
                model: 2
                Rectangle {
                    required property int index
                    readonly property bool isLo: index === 0
                    width: 10; height: 18; radius: 3
                    x: (isLo ? range.lo : range.hi) * range.width - width / 2
                    color: handle.pressed ? Theme.hover : Theme.raised
                    border.color: Theme.text
                    Text {
                        anchors.centerIn: parent
                        text: parent.isLo ? "L" : "H"
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: 8; weight: Font.DemiBold }
                    }
                    MouseArea {
                        id: handle
                        anchors.fill: parent
                        anchors.margins: -4
                        preventStealing: true
                        onPositionChanged: (m) => {
                            if (!pressed) return
                            const p = mapToItem(range, m.x, m.y)
                            const v = Math.max(0, Math.min(1, p.x / range.width))
                            if (parent.isLo) range.lo = v
                            else range.hi = v
                        }
                        onReleased: smart.setRangePosition(row.index, range.lo, range.hi)
                    }
                }
            }
        }

        Text {
            id: hiText
            anchors { right: invert.left; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
            width: 64
            text: row.modelData.hiText
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
        }
        SButton {
            id: invert
            anchors { right: remove.left; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
            text: "Invert"
            implicitHeight: 22
            active: row.modelData.invert
            activeColour: Theme.caution
            onClicked: smart.setInvert(row.index, !row.modelData.invert)
        }
        SButton {
            id: remove
            anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
            text: "×"
            implicitHeight: 22
            onClicked: smart.removeMapping(row.index)
        }
    }
}
