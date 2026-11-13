// SPDX-License-Identifier: GPL-3.0-or-later
//
// A rotary control for the generic plugin editor: a ring of dots lit from
// the origin to the value, as a hardware encoder's LED collar, around a
// cap with a pointer. Vertical drag moves it, 200 px for the whole travel;
// Ctrl held drags at a tenth of that for fine work (Pro Tools' fine
// adjust); Alt-click returns it to its default.
//
// The knob keeps no value of its own: it reports positions and draws the
// one it is given, so what it shows is what the model last accepted.
import QtQuick
import Sonora

Item {
    id: knob
    property real position: 0     // 0..1, from the model
    property real origin: 0       // where the lit arc starts
    property real defaultPosition: 0
    property int diameter: 52
    // What a screen reader says; the panel names the parameter.
    property string accessibleName: ""
    property string accessibleValue: Math.round(position * 100) + " %"
    signal begin()
    signal moved(real position)
    signal end()
    signal reset()

    readonly property int dots: 31
    readonly property real sweep: 270
    readonly property bool dragging: area.pressed && area.dragging

    implicitWidth: diameter + 12
    implicitHeight: diameter + 12

    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: accessibleName
    Accessible.description: accessibleValue
    Accessible.onIncreaseAction: knob.step(0.01)
    Accessible.onDecreaseAction: knob.step(-0.01)

    // One press is one begin/moved/end, so one undo step; a press at the end
    // of the travel changes nothing and opens no step.
    function step(delta) {
        const next = Math.max(0, Math.min(1, knob.position + delta))
        if (next === knob.position) return
        knob.begin()
        knob.moved(next)
        knob.end()
    }

    function stepSize(modifiers) {
        if (modifiers & Qt.ControlModifier) return 0.001
        if (modifiers & Qt.ShiftModifier) return 0.1
        return 0.01
    }

    Keys.onPressed: (e) => {
        if (e.modifiers & (Qt.AltModifier | Qt.MetaModifier)) return
        switch (e.key) {
        case Qt.Key_Up:
        case Qt.Key_Right: knob.step(stepSize(e.modifiers)); break
        case Qt.Key_Down:
        case Qt.Key_Left: knob.step(-stepSize(e.modifiers)); break
        case Qt.Key_PageUp: knob.step(0.1); break
        case Qt.Key_PageDown: knob.step(-0.1); break
        case Qt.Key_Home: knob.reset(); break
        default: return
        }
        e.accepted = true
    }

    // Keyboard focus ring.
    Rectangle {
        objectName: "knobFocusRing"
        anchors.centerIn: parent
        width: knob.width - 2
        height: width
        radius: width / 2
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
        visible: knob.activeFocus
    }

    function angleAt(p) { return -sweep / 2 + Math.max(0, Math.min(1, p)) * sweep }

    // The collar.
    Repeater {
        model: knob.dots
        Rectangle {
            required property int index
            readonly property real at: index / (knob.dots - 1)
            readonly property real lo: Math.min(knob.origin, knob.position)
            readonly property real hi: Math.max(knob.origin, knob.position)
            readonly property real half: 0.5 / (knob.dots - 1)
            readonly property bool lit: at >= lo - half && at <= hi + half
            readonly property real rad: knob.angleAt(at) * Math.PI / 180
            readonly property real ring: knob.width / 2 - 3
            width: 4; height: 4; radius: 2
            x: knob.width / 2 + ring * Math.sin(rad) - width / 2
            y: knob.height / 2 - ring * Math.cos(rad) - height / 2
            color: lit ? Theme.accent : Theme.borderStrong
        }
    }

    // The default: a faint dot just outside the ring, so it never hides a
    // lit one.
    Rectangle {
        readonly property real rad: knob.angleAt(knob.defaultPosition) * Math.PI / 180
        readonly property real ring: knob.width / 2 + 2
        width: 3; height: 3; radius: 1.5
        x: knob.width / 2 + ring * Math.sin(rad) - width / 2
        y: knob.height / 2 - ring * Math.cos(rad) - height / 2
        color: Theme.textTertiary
    }

    Rectangle {
        id: cap
        anchors.centerIn: parent
        width: knob.diameter - 6
        height: width
        radius: width / 2
        border.color: area.containsMouse || knob.dragging ? Theme.borderStrong : Theme.borderSubtle
        gradient: Gradient {
            GradientStop { position: 0.0; color: area.containsMouse || knob.dragging ? Theme.hover : Theme.raised }
            GradientStop { position: 1.0; color: Theme.panel }
        }
        // The pointer, from the centre out.
        Item {
            anchors.fill: parent
            rotation: knob.angleAt(knob.position)
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 4
                width: 2
                height: cap.height / 2 - 6
                radius: 1
                color: Theme.text
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        // The panel scrolls vertically; a knob's drag is the knob's.
        preventStealing: true
        property bool dragging: false
        property real lastY: 0
        property real at: 0
        onPressed: (m) => {
            if (m.modifiers & Qt.AltModifier) {
                dragging = false
                knob.reset()
                return
            }
            knob.forceActiveFocus()
            dragging = true
            lastY = m.y
            at = knob.position
            knob.begin()
        }
        onPositionChanged: (m) => {
            if (!pressed || !dragging) return
            const scale = (m.modifiers & Qt.ControlModifier) ? 0.1 : 1.0
            at = Math.max(0, Math.min(1, at + (lastY - m.y) / 200 * scale))
            lastY = m.y
            knob.moved(at)
        }
        onReleased: {
            if (dragging) knob.end()
            dragging = false
        }
        onCanceled: {
            if (dragging) knob.end()
            dragging = false
        }
    }
}
