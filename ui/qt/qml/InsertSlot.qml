// SPDX-License-Identifier: GPL-3.0-or-later
//
// One insert slot, A to J, on a channel strip. What it shows, loudest
// first (brain/specs/ui-design.md, "Insert slot"): a fault (alarm, with the
// engine's reason on hover and in the panel), bypass (filled in `bypass`),
// inactive (name in italics on the strip's own surface), running (raised),
// empty (a sunken well with the slot's letter).
//
// Click an empty slot to choose a plugin; click a plugin to open its
// parameters; Ctrl-click bypasses, Ctrl+Start-click makes it inactive;
// right-click for the slot's menu. Drag to another slot or strip to move
// it there, Alt-drag to copy it (InsertModel, insert_controller.h).
import QtQuick
import Sonora

Rectangle {
    id: s
    property InsertModel inserts: null
    property Item dragHost: null
    property int track: 0
    property int slot: 0
    property var info: ({ present: false, letter: "?" })
    readonly property bool present: info.present === true
    readonly property bool dropTarget: dragHost !== null && dragHost.dragging
                                       && dragHost.targetTrack === track && dragHost.targetSlot === slot

    objectName: "insert_" + track + "_" + slot
    height: Theme.insertSlotHeight
    radius: Theme.radiusSmall
    color: !present ? Theme.sunken
           : info.faulted ? Theme.alarmSurface
           : !info.active ? Theme.panel
           : info.bypass ? Theme.bypass
           : (area.containsMouse ? Theme.hover : Theme.raised)
    border.width: 1
    border.color: dropTarget ? Theme.accent
                  : present && info.faulted ? Theme.alarm
                  : Theme.borderSubtle

    // The light edge of a raised control, on a plugin that runs.
    Rectangle {
        visible: s.present && s.info.active && !s.info.bypass && !s.info.faulted
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
        height: 1
        color: Theme.highlight
    }

    Text {
        anchors { fill: parent; leftMargin: Theme.space1; rightMargin: Theme.space1 }
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: s.present ? Text.AlignLeft : Text.AlignHCenter
        elide: Text.ElideRight
        // A DemiBold caption letter is up to about 6.5 px wide: the name, or
        // its short form (short_plugin_name) where the strip is too narrow.
        readonly property int chars: Math.floor(width / 6.5) - (s.present && s.info.faulted ? 2 : 0)
        text: !s.present ? s.info.letter
              : (s.info.faulted ? "! " : "")
                + (s.inserts !== null ? s.inserts.shortName(s.info.name, chars) : s.info.name)
        color: !s.present ? Theme.textTertiary
               : s.info.faulted ? Theme.alarm
               : !s.info.active ? Theme.textSecondary
               : s.info.bypass ? Theme.textOnAccent
               : Theme.text
        font {
            family: Theme.fontFamily
            pixelSize: Theme.textCaption
            italic: s.present && !s.info.active
            weight: s.present && s.info.active ? Font.DemiBold : Font.Normal
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        property point pressAt
        property bool dragged: false
        // The whole name, and what is wrong, under the pointer.
        onContainsMouseChanged: {
            if (s.dragHost === null) return
            if (!containsMouse || !s.present) {
                s.dragHost.hideHint(s.track, s.slot)
                return
            }
            const p = mapToItem(s.dragHost, 0, height)
            s.dragHost.showHint(s.track, s.slot,
                                s.info.name + (!s.info.active ? " (inactive)" : s.info.bypass ? " (bypassed)" : "")
                                + (s.info.faulted ? "\n" + s.info.fault : ""),
                                p.x, p.y, s.info.faulted)
        }
        onPressed: (m) => {
            pressAt = Qt.point(m.x, m.y)
            dragged = false
        }
        onPositionChanged: (m) => {
            if (!pressed || !s.present || s.dragHost === null || (m.buttons & Qt.LeftButton) === 0) return
            if (!dragged && Math.abs(m.x - pressAt.x) + Math.abs(m.y - pressAt.y) < 4) return
            const p = mapToItem(s.dragHost, m.x, m.y)
            if (!dragged) {
                dragged = true
                s.dragHost.beginDrag(s.track, s.slot, s.info.name)
            }
            s.dragHost.dragTo(p.x, p.y, (m.modifiers & Qt.AltModifier) !== 0)
        }
        // `dragged` stays set until the next press, so the click that ends
        // a drag is not also taken as a click on the slot.
        onReleased: (m) => {
            if (dragged) s.dragHost.endDrag((m.modifiers & Qt.AltModifier) !== 0)
        }
        onCanceled: if (dragged && s.dragHost !== null) s.dragHost.cancelDrag()
        onClicked: (m) => {
            if (s.inserts === null || dragged) return
            if (m.button === Qt.RightButton) {
                if (s.dragHost !== null) {
                    const p = mapToItem(s.dragHost, m.x, m.y)
                    s.dragHost.openMenu(s.track, s.slot, p.x, p.y)
                }
                return
            }
            s.inserts.slotClicked(s.track, s.slot, m.modifiers)
        }
    }
}
