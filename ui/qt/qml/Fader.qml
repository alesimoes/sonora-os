// SPDX-License-Identifier: GPL-3.0-or-later
//
// A vertical fader for the generic plugin editor, for a parameter whose
// plugin asked for one (ADR-0057, PluginWidget::Fader): a slot, a cap and the
// lit part of the slot from `origin` to the value. Like the knob it keeps no
// value of its own, drag moves it (Ctrl for a tenth), Alt-click resets.
import QtQuick
import Sonora

Item {
    id: fader
    property real position: 0
    property real origin: 0
    property real defaultPosition: 0
    property int travel: 72
    signal begin()
    signal moved(real position)
    signal end()
    signal reset()

    implicitWidth: 32
    implicitHeight: travel + 20

    readonly property real topPad: 10
    function yAt(p) { return topPad + (1 - Math.max(0, Math.min(1, p))) * travel }

    Rectangle {
        id: slot
        x: parent.width / 2 - 2
        y: fader.topPad
        width: 4
        height: fader.travel
        radius: 2
        color: Theme.sunken
        border.color: Theme.borderSubtle
    }
    Rectangle {
        x: slot.x
        width: slot.width
        radius: 2
        y: Math.min(fader.yAt(fader.position), fader.yAt(fader.origin))
        height: Math.abs(fader.yAt(fader.position) - fader.yAt(fader.origin))
        color: Theme.accent
    }
    Rectangle {
        x: slot.x - 5
        width: 14
        y: fader.yAt(fader.defaultPosition) - 0.5
        height: 1
        color: Theme.textTertiary
    }
    Rectangle {
        id: cap
        x: parent.width / 2 - 9
        width: 18
        height: 10
        radius: 2
        y: fader.yAt(fader.position) - height / 2
        color: area.containsMouse || area.dragging ? Theme.hover : Theme.raised
        border.color: area.containsMouse || area.dragging ? Theme.borderStrong : Theme.borderSubtle
        Rectangle { anchors.centerIn: parent; width: parent.width - 6; height: 1; color: Theme.text }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        property bool dragging: false
        property real lastY: 0
        property real at: 0
        onPressed: (m) => {
            if (m.modifiers & Qt.AltModifier) {
                dragging = false
                fader.reset()
                return
            }
            dragging = true
            lastY = m.y
            at = fader.position
            fader.begin()
        }
        onPositionChanged: (m) => {
            if (!pressed || !dragging) return
            const scale = (m.modifiers & Qt.ControlModifier) ? 0.1 : 1.0
            at = Math.max(0, Math.min(1, at + (lastY - m.y) / fader.travel * scale))
            lastY = m.y
            fader.moved(at)
        }
        onReleased: { if (dragging) fader.end(); dragging = false }
        onCanceled: { if (dragging) fader.end(); dragging = false }
    }
}
