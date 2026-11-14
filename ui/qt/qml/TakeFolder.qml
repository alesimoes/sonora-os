// SPDX-License-Identifier: GPL-3.0-or-later
// One track's take folder: the comp as a row, each stretch coloured by the
// lane it came from. `segments` is a list of {start, end, colour} from
// TakeFolderView; a colour of -1 (no lane holds the audio) draws grey.
import QtQuick 2.15

Item {
    id: root
    property var segments: []
    property bool expanded: false
    property real scrollFrames: 0
    property real framesPerPixel: 1
    property var palette: ["#d9534f", "#f0ad4e", "#e5d44a", "#5cb85c",
                           "#3fb8af", "#5b9bd5", "#8e6fd1", "#d16fb0"]
    signal toggled()

    height: 24
    clip: true

    Repeater {
        model: root.segments
        Rectangle {
            x: (modelData.start - root.scrollFrames) / root.framesPerPixel
            width: Math.max(1, (modelData.end - modelData.start) / root.framesPerPixel)
            height: root.height - 4
            y: 2
            radius: 2
            color: modelData.colour >= 0 ? root.palette[modelData.colour] : "#777777"
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.toggled()
    }
}
