// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drag snap, key, scale, amount and Snap Notes.
// A fragment of PitchPanel.qml, spliced in at build time by qt/CMakeLists.txt
// where PitchPanel.qml says //#include "PitchScaleBar.qml"; it uses the ids pitchPanel,
// face and body from there, so it is not a standalone component.

Item {
    id: scaleBar
    visible: pitchPanel.available
    width: body.width
    height: 50

    Row {
        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
        spacing: Theme.space4

        Column {
            spacing: 4
            Text { text: "DRAG SNAP"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Row {
                spacing: 2
                Repeater {
                    model: ["Off", "Semitone", "Scale"]
                    SButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        active: pitchPanel.snapMode === index
                        implicitWidth: 74
                        onClicked: pitchPanel.snapModeChosen(index)
                    }
                }
            }
        }
        Column {
            spacing: 4
            Text { text: "KEY"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Row {
                spacing: 2
                Repeater {
                    model: pitchPanel.keyNames
                    SButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        active: pitchPanel.key === index
                        implicitWidth: 30
                        onClicked: pitchPanel.keyChosen(index)
                    }
                }
            }
        }
        Column {
            spacing: 4
            Text { text: "SCALE"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Row {
                spacing: 2
                SButton {
                    text: "<"
                    implicitWidth: 24
                    onClicked: pitchPanel.scaleChosen((pitchPanel.scale + pitchPanel.scaleNames.length - 1) % Math.max(1, pitchPanel.scaleNames.length))
                }
                Rectangle {
                    width: 132
                    height: 24
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: Theme.borderStrong
                    Text {
                        anchors.centerIn: parent
                        text: pitchPanel.scaleNames.length > pitchPanel.scale ? pitchPanel.scaleNames[pitchPanel.scale] : ""
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                    }
                }
                SButton {
                    text: ">"
                    implicitWidth: 24
                    onClicked: pitchPanel.scaleChosen((pitchPanel.scale + 1) % Math.max(1, pitchPanel.scaleNames.length))
                }
            }
        }
        Column {
            spacing: 4
            Text { text: "AMOUNT  " + Math.round(pitchPanel.amount) + "%"; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            Item {
                id: amountBar
                width: 120
                height: 24
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Theme.sunken
                    border.color: Theme.borderSubtle
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: pitchPanel.amount / 100 * parent.width
                    height: 4
                    radius: 2
                    color: Theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: pitchPanel.canEdit
                    function set(x) { pitchPanel.amountMoved(Math.round(100 * Math.max(0, Math.min(1, x / width)))) }
                    onPressed: (mouse) => set(mouse.x)
                    onPositionChanged: (mouse) => set(mouse.x)
                }
            }
        }
        Column {
            spacing: 4
            Text { text: " "; color: Theme.textTertiary; font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 } }
            SButton {
                text: "Snap Notes"
                available: pitchPanel.canEdit
                implicitWidth: 92
                onClicked: pitchPanel.snapNotes()
            }
        }
    }
}
