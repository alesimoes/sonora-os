// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one button. States (brain/specs/ui-design.md, "Component states"):
// rest, hover, active (lit in its meaning colour, dark text), and
// unavailable (the command is not in this build: dimmed, still clickable,
// so the status line can say why). Every value is a Theme token.
import QtQuick
import Sonora

Rectangle {
    id: b
    property string text
    property bool active: false
    property bool available: true
    property color activeColour: Theme.accent
    property int fontPx: Theme.textSmall
    property bool bold: true
    signal clicked(var mouse)

    implicitWidth: Math.max(implicitHeight, label.implicitWidth + 2 * Theme.space2)
    implicitHeight: 24
    radius: Theme.radius
    color: active ? activeColour : (area.containsMouse && available ? Theme.hover : Theme.raised)
    border.color: active ? Qt.darker(activeColour, 1.15) : Theme.borderSubtle
    border.width: 1
    opacity: available ? 1.0 : 0.45

    // Subtle depth: a hairline of light along the top edge.
    Rectangle {
        visible: !b.active
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
        height: 1
        radius: b.radius
        color: Theme.highlight
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: b.text
        color: b.active ? Theme.textOnAccent : Theme.text
        font { family: Theme.fontFamily; pixelSize: b.fontPx; weight: b.bold ? Font.DemiBold : Font.Normal }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: (mouse) => b.clicked(mouse)
    }
}
