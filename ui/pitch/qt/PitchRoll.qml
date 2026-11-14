// SPDX-License-Identifier: GPL-3.0-or-later
//
// The piano ruler and the note canvas.
// A fragment of PitchPanel.qml, spliced in at build time by qt/CMakeLists.txt
// where PitchPanel.qml says //#include "PitchRoll.qml"; it uses the ids pitchPanel,
// face and body from there, so it is not a standalone component.

    Rectangle {
        id: rollFace
        width: pitchPanel.canvasWidth + 58
        height: pitchPanel.canvasHeight + 2
        color: Theme.sunken
        border.color: Theme.borderSubtle
        radius: Theme.radiusSmall
        clip: true

        // A keyboard on its side: ivory throughout, the black keys
        // shorter on top, a seam under every white row.
        Rectangle {
            id: ruler
            x: 1; y: 1
            width: 56
            height: pitchPanel.canvasHeight
            color: "#d9dbde"
            Repeater {
                model: pitchPanel.rows
                Rectangle {
                    required property var modelData
                    y: modelData.y
                    width: modelData.black ? 34 : ruler.width
                    height: modelData.h
                    color: modelData.black ? "#1d2024" : "transparent"
                    radius: modelData.black ? 2 : 0
                    Rectangle {
                        visible: !modelData.black
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: "#9ba0a6"
                    }
                    Text {
                        visible: modelData.name.length > 0
                        anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                        text: modelData.name
                        color: "#30343a"
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
                    }
                }
            }
        }

        Item {
            id: roll
            objectName: "pitchRoll"
            x: 58; y: 1
            width: pitchPanel.canvasWidth
            height: pitchPanel.canvasHeight

            // One lane per semitone, the black keys' darker, a line on each C.
            Repeater {
                model: pitchPanel.rows
                Rectangle {
                    required property var modelData
                    y: modelData.y
                    width: roll.width
                    height: modelData.h
                    color: modelData.black ? Qt.darker(Theme.sunken, 1.18) : "transparent"
                    Rectangle {
                        visible: modelData.name.length > 0
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.borderStrong
                    }
                }
            }

            // Where each moved note was found.
            Repeater {
                model: pitchPanel.notes
                Rectangle {
                    required property var modelData
                    visible: modelData.moved
                    x: modelData.x; y: modelData.originY
                    width: modelData.w; height: modelData.h
                    radius: height / 2
                    color: "transparent"
                    border.color: Theme.textTertiary
                    border.width: 1
                    opacity: 0.8
                }
            }

            Repeater {
                model: pitchPanel.notes
                Rectangle {
                    required property var modelData
                    x: modelData.x; y: modelData.y
                    width: modelData.w; height: modelData.h
                    radius: height / 2
                    color: modelData.selected ? Theme.accent : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.55)
                    border.color: modelData.selected ? Theme.highlight : Qt.darker(Theme.accent, 1.4)
                    border.width: modelData.selected ? 2 : 1
                    Text {
                        visible: parent.width > implicitWidth + 8
                        anchors { left: parent.left; leftMargin: 6; bottom: parent.top; bottomMargin: 1 }
                        text: modelData.label
                        color: modelData.selected ? Theme.text : Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                    }
                }
            }

            // The pitch line through each note, as it will sound.
            Canvas {
                id: contours
                anchors.fill: parent
                onPaint: {
                    const g = getContext("2d")
                    g.reset()
                    g.lineWidth = 1.5
                    g.strokeStyle = "#eef1f3"
                    for (const n of pitchPanel.notes) {
                        const c = n.contour
                        if (!c || c.length < 4) continue
                        g.beginPath()
                        g.moveTo(c[0], c[1])
                        for (let k = 2; k + 1 < c.length; k += 2) g.lineTo(c[k], c[k + 1])
                        g.stroke()
                    }
                }
                Connections {
                    target: pitchPanel
                    function onNotesChanged() { contours.requestPaint() }
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: pitchPanel.canEdit
                property real startY: 0
                onPressed: (mouse) => { startY = mouse.y; pitchPanel.pressedAt(mouse.x, mouse.y) }
                onPositionChanged: (mouse) => pitchPanel.draggedBy(mouse.y - startY)
                onReleased: pitchPanel.released()
            }
        }
    }
