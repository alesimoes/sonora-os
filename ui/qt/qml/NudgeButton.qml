// SPDX-License-Identifier: GPL-3.0-or-later
//
// NUDGE value beside GRID (ADR-0149). Shows the value; a click runs
// nudge.value.next. `label` and `onCycle` are bound by the toolbar.
import QtQuick

Rectangle {
    id: root
    property string label: "GRID"
    signal cycle()
    implicitWidth: 92
    implicitHeight: 24
    radius: 3
    color: "#2b2f36"
    border.color: "#4a505a"
    Row {
        anchors.centerIn: parent
        spacing: 6
        Text { text: "NUDGE"; color: "#8d95a3"; font.pixelSize: 10 }
        Text { text: root.label; color: "#e6e9ef"; font.pixelSize: 11; font.bold: true }
    }
    MouseArea { anchors.fill: parent; onClicked: root.cycle() }
}
