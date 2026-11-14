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
    // The entry point: name the region being edited and show the panel.
    function open(region) {
        regionName = region
        show()
    }
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
            //#include "PitchRoll.qml"
            //#include "PitchInspector.qml"
            }

            //#include "PitchScaleBar.qml"

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
