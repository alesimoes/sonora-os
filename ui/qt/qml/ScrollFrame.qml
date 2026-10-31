// SPDX-License-Identifier: GPL-3.0-or-later
//
// A clipped frame a strip section scrolls inside when the fader's share of
// the strip (ui-design.md, "Mixer strip anatomy") leaves it less room than
// its content needs. The wheel moves it; dragging does not, because a drag
// on an insert slot is a move between strips. A thin bar on the right edge
// says there is more, so a slot out of sight is never mistaken for absent.
import QtQuick
import Sonora

Flickable {
    id: frame
    clip: true
    interactive: false
    boundsBehavior: Flickable.StopAtBounds
    contentWidth: width
    readonly property bool overflows: contentHeight > height + 0.5

    WheelHandler {
        enabled: frame.overflows
        onWheel: (e) => {
            const max = frame.contentHeight - frame.height
            frame.contentY = Math.max(0, Math.min(max, frame.contentY - e.angleDelta.y / 4))
        }
    }
    Rectangle {
        objectName: "scrollBar"
        visible: frame.overflows
        parent: frame
        z: 5
        x: frame.width - width
        y: frame.contentHeight > 0 ? frame.contentY / frame.contentHeight * frame.height : 0
        width: 2
        height: frame.contentHeight > 0 ? frame.height * frame.height / frame.contentHeight : 0
        radius: 1
        color: Theme.textTertiary
    }
}
