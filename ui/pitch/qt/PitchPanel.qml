// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pitch (time.pitch, ADR-0143): the notes of one monophonic region as blobs
// over a piano ruler. Drag a blob to move the note, set its drift, vibrato,
// formant and gain in the inspector, pull every note toward a key, and Apply
// renders a copy in one undo step. The panel keeps no state of its own: it
// draws what PitchEditorModel (ui/pitch) says, geometry included, and reports
// gestures as signals, so what it shows is what the model last accepted.
// A refusal stays in the notice band until the operator dismisses it; with
// no shifter in the engine the editor is replaced by the reason.
import QtQuick
import Sonora

Item {
    id: pitchPanel
    property bool available: false
    property string reason: ""
    property string regionName: ""
    property string status: ""
    property string notice: ""
    // note_blobs(): { x, y, w, h, originY, moved, selected, label, contour }
    property var notes: []
    // ruler_rows(): { y, h, name, black }
    property var rows: []
    // The selected note, or { none: true }: { name, found, shift, straighten,
    // vibrato, formant, gain } with the knobs' values in their own units.
    property var inspector: ({ none: true })
    property var keyNames: []
    property var scaleNames: []
    property int snapMode: 1    // 0 off, 1 semitone, 2 scale
    property int key: 0
    property int scale: 1
    property real amount: 100
    property bool canApply: false
    property bool canEdit: true
    readonly property int canvasWidth: 780
    readonly property int canvasHeight: 400

    signal pressedAt(real x, real y)
    signal draggedBy(real dy)
    signal released()
    signal straightenMoved(real percent)
    signal vibratoMoved(real percent)
    signal formantMoved(int cents)
    signal gainMoved(real db)
    signal snapModeChosen(int mode)
    signal keyChosen(int key)
    signal scaleChosen(int scale)
    signal amountMoved(real percent)
    signal snapNotes()
    signal applyClicked()
    signal resetClicked()
    signal noticeDismissed()
    signal closed()

    visible: false
    anchors.fill: parent
    z: 9

    function show() { visible = true }
    function close() {
        visible = false
        closed()
    }
    function signed(v, digits) {
        const s = Number(v).toFixed(digits)
        return v > 0 ? "+" + s : s
    }

    MouseArea { anchors.fill: parent }
    Rectangle {
        anchors.fill: parent
        color: Theme.shadow
        opacity: 0.45
    }

    Rectangle {
        id: face
        objectName: "pitchPanel"
        anchors.centerIn: parent
        width: 1200
        height: body.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong

        Column {
            id: body
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space3

            // Title strip, as a rack unit's: name left, what it is working on right.
            Item {
                width: parent.width
                height: 28
                Text {
                    id: title
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    text: "Note Pitch"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
                }
                Text {
                    anchors { left: title.right; leftMargin: Theme.space3; baseline: title.baseline }
                    text: pitchPanel.regionName
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Text {
                    objectName: "pitchStatus"
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    text: pitchPanel.status
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }

            // The held notice: a refusal stays until dismissed.
            Rectangle {
                objectName: "pitchNotice"
                visible: pitchPanel.notice.length > 0
                width: parent.width
                height: visible ? Math.max(36, noticeText.implicitHeight + 2 * Theme.space2) : 0
                radius: Theme.radiusSmall
                color: Theme.alarmSurface
                border.color: Theme.caution
                Text {
                    id: noticeText
                    anchors { left: parent.left; right: dismiss.left; margins: Theme.space3; verticalCenter: parent.verticalCenter }
                    wrapMode: Text.Wrap
                    text: pitchPanel.notice
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                SButton {
                    id: dismiss
                    anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    text: "Dismiss"
                    implicitWidth: 76
                    onClicked: pitchPanel.noticeDismissed()
                }
            }

            // No shifter: the reason, and nothing that would pretend to edit.
            Rectangle {
                objectName: "pitchOff"
                visible: !pitchPanel.available
                width: parent.width
                height: visible ? 220 : 0
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                Column {
                    anchors.centerIn: parent
                    width: parent.width * 0.6
                    spacing: Theme.space2
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "Pitch editing is off"
                        color: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
                    }
                    Text {
                        objectName: "pitchOffReason"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: pitchPanel.reason
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    }
                }
            }

            Row {
                visible: pitchPanel.available
                spacing: Theme.space3
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
                Rectangle {
                    id: inspectorFace
                    width: face.width - 2 * Theme.space4 - pitchPanel.canvasWidth - 58 - Theme.space3
                    height: pitchPanel.canvasHeight + 2
                    radius: Theme.radiusSmall
                    color: Theme.raised
                    border.color: Theme.borderStrong
                    readonly property var ins: pitchPanel.inspector
                    readonly property bool none: !ins || ins.none === true

                    Column {
                        anchors { fill: parent; margins: Theme.space3 }
                        spacing: Theme.space3

                        Rectangle {
                            width: parent.width
                            height: 74
                            radius: Theme.radiusSmall
                            color: "#0f1418"
                            border.color: Theme.borderSubtle
                            Text {
                                objectName: "pitchReadout"
                                anchors { left: parent.left; leftMargin: Theme.space3; top: parent.top; topMargin: Theme.space2 }
                                text: inspectorFace.none ? "--" : inspectorFace.ins.name
                                color: "#8fe3c0"
                                font { family: Theme.fontFamily; pixelSize: Theme.textDisplay; weight: Font.DemiBold; features: { "tnum": 1 } }
                            }
                            Text {
                                anchors { right: parent.right; rightMargin: Theme.space3; top: parent.top; topMargin: Theme.space2 + 4 }
                                text: inspectorFace.none ? "" : pitchPanel.signed(inspectorFace.ins.shift, 0) + " ct"
                                color: "#8fe3c0"
                                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; features: { "tnum": 1 } }
                            }
                            Text {
                                anchors { left: parent.left; leftMargin: Theme.space3; bottom: parent.bottom; bottomMargin: Theme.space2 }
                                text: inspectorFace.none ? "Select a note to edit it" : "found " + inspectorFace.ins.found
                                color: "#5d8f7a"
                                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                            }
                        }

                        // Value, knob position, lit-arc origin; the signal per index below.
                        Grid {
                            columns: 2
                            columnSpacing: Theme.space3
                            rowSpacing: Theme.space2
                            opacity: inspectorFace.none ? 0.4 : 1.0
                            Repeater {
                                model: {
                                    const i = inspectorFace.ins
                                    const none = inspectorFace.none
                                    return [
                                        { label: "STRAIGHTEN", value: none ? "" : Math.round(i.straighten) + "%",
                                          position: none ? 0 : i.straighten / 100, origin: 0 },
                                        { label: "VIBRATO", value: none ? "" : Math.round(i.vibrato) + "%",
                                          position: none ? 0.5 : i.vibrato / 200, origin: 0.5 },
                                        { label: "FORMANT", value: none ? "" : pitchPanel.signed(i.formant, 0) + " ct",
                                          position: none ? 0.5 : 0.5 + i.formant / 4800, origin: 0.5 },
                                        { label: "GAIN", value: none ? "" : pitchPanel.signed(i.gain, 1) + " dB",
                                          position: none ? 0.8 : (i.gain + 48) / 60, origin: 0.8 }
                                    ]
                                }
                                Column {
                                    id: encoder
                                    required property int index
                                    required property var modelData
                                    width: 128
                                    spacing: 2
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: encoder.modelData.label
                                        color: Theme.textSecondary
                                        font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 }
                                    }
                                    Knob {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        diameter: 50
                                        enabled: pitchPanel.canEdit && !inspectorFace.none
                                        position: encoder.modelData.position
                                        origin: encoder.modelData.origin
                                        defaultPosition: encoder.modelData.origin
                                        onMoved: (p) => {
                                            if (encoder.index === 0) pitchPanel.straightenMoved(Math.round(p * 100))
                                            else if (encoder.index === 1) pitchPanel.vibratoMoved(Math.round(p * 200))
                                            else if (encoder.index === 2) pitchPanel.formantMoved(Math.round((p - 0.5) * 4800))
                                            else pitchPanel.gainMoved(Math.round(p * 600 - 480) / 10)
                                        }
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: encoder.modelData.value
                                        color: Theme.text
                                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                                    }
                                }
                            }
                        }
                    }
                }
            }

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

            // Reset and Apply on a line of their own, at the right; Close
            // stays when the editor is off, so the panel always has a way out.
            Item {
                width: body.width
                height: 28
                Row {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    spacing: Theme.space2
                    SButton {
                        objectName: "pitchReset"
                        visible: pitchPanel.available
                        text: "Reset"
                        available: pitchPanel.canEdit
                        implicitWidth: 76
                        onClicked: pitchPanel.resetClicked()
                    }
                    SButton {
                        text: "Close"
                        implicitWidth: 76
                        onClicked: pitchPanel.close()
                    }
                    SButton {
                        objectName: "pitchApply"
                        visible: pitchPanel.available
                        text: "Apply"
                        active: pitchPanel.canApply
                        available: pitchPanel.canApply
                        implicitWidth: 76
                        onClicked: pitchPanel.applyClicked()
                    }
                }
            }
        }
    }
}
