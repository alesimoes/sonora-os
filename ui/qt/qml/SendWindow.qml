// SPDX-License-Identifier: GPL-3.0-or-later
//
// The send window, Pro Tools' (ADR-0022): one send's level fader and
// readout, its pan, PRE (pre-fader), M (the send's own mute), FMP (follow
// the strip's main pan; a pan dragged here turns it off) and the bus it
// goes to, which opens the chooser to send it elsewhere. Fader and pan
// behave as a strip's: drag, Ctrl for fine, Alt-click for 0 dB or centre,
// click a readout to type. Values are mix state: no undo step.
import QtQuick
import Sonora

Rectangle {
    id: w
    required property ShellModel shell
    property SendModel sends: null
    required property SessionModel session
    readonly property var info: sends !== null && sends.windowOpen ? sends.window : ({})

    width: 200
    height: 360
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong

    Text {
        id: title
        objectName: "sendWindowTitle"
        anchors { left: parent.left; right: close.left; top: parent.top; margins: Theme.space2 }
        elide: Text.ElideRight
        text: w.info.title || ""
        color: Theme.text
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
    }
    SButton {
        id: close
        objectName: "sendWindowClose"
        anchors { right: parent.right; top: parent.top; margins: Theme.space1 }
        text: "×"; implicitWidth: 20; implicitHeight: 20; bold: false
        onClicked: w.sends.closeWindow()
    }

    SButton {
        id: busButton
        objectName: "sendWindowBus"
        anchors { left: parent.left; right: parent.right; top: title.bottom; margins: Theme.space2 }
        implicitHeight: 22
        bold: false
        fontPx: Theme.textSmall
        text: w.info.bus || ""
        onClicked: w.sends.reassign()
    }

    Row {
        id: toggles
        anchors { horizontalCenter: parent.horizontalCenter; top: busButton.bottom; topMargin: Theme.space2 }
        spacing: Theme.space1
        SButton {
            objectName: "sendWindowPre"
            text: "PRE"; implicitHeight: 20; fontPx: Theme.textCaption
            active: w.info.pre === true
            onClicked: w.sends.togglePre()
        }
        SButton {
            objectName: "sendWindowMute"
            text: "M"; implicitHeight: 20; fontPx: Theme.textCaption
            active: w.info.mute === true; activeColour: Theme.mute
            onClicked: w.sends.toggleMute()
        }
        SButton {
            objectName: "sendWindowFmp"
            text: "FMP"; implicitHeight: 20; fontPx: Theme.textCaption
            active: w.info.followPan === true
            onClicked: w.sends.toggleFollowPan()
        }
    }

    Item {
        id: pan
        anchors { left: parent.left; right: parent.right; top: toggles.bottom; margins: Theme.space2 }
        height: 30
        Rectangle {
            id: panTrack
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 8; radius: Theme.radiusSmall
            color: Theme.sunken
            border.color: Theme.borderSubtle
            opacity: w.info.followPan === true ? 0.5 : 1.0
            Rectangle {
                readonly property real c: panTrack.width / 2
                readonly property real p: w.info.pan || 0
                x: p < 0 ? c + p * c : c
                width: Math.max(2, Math.abs(p) * c)
                height: parent.height
                radius: Theme.radiusSmall
                color: Theme.accent
            }
            Rectangle { x: panTrack.width / 2; width: 1; height: parent.height; color: Theme.textTertiary }
        }
        ValueField {
            id: panField
            objectName: "sendWindowPan"
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
            width: 60; height: 16
            shell: w.shell
            text: w.info.followPan === true ? "FMP" : (w.info.panText || "")
            textColour: Theme.textSecondary
            fontPx: Theme.textCaption
            onSubmitted: (t) => { if (w.sends.setPanText(t)) panField.finish() }
        }
        MouseArea {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 14
            property real lastX: 0
            onPressed: (m) => {
                lastX = m.x
                if (m.modifiers & Qt.AltModifier) w.sends.setPanText("C")
            }
            onPositionChanged: (m) => {
                if (!pressed || (m.modifiers & Qt.AltModifier)) return
                w.sends.panDrag(m.x - lastX, width, (m.modifiers & Qt.ControlModifier) !== 0)
                lastX = m.x
            }
        }
    }

    Item {
        id: faderArea
        anchors { left: parent.left; right: parent.right; top: pan.bottom; bottom: readout.top
                  margins: Theme.space2 }
        readonly property real capH: 26
        readonly property real travel: height - capH
        Repeater {
            model: [12, 6, 0, -5, -10, -20, -30, -40, -60]
            Text {
                required property int modelData
                x: faderArea.width / 2 - 34
                y: faderArea.capH / 2 + (1 - w.session.faderPosition(modelData)) * faderArea.travel - height / 2
                text: modelData > 0 ? "+" + modelData : modelData
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textOverline; features: { "tnum": 1 } }
            }
        }
        Rectangle {
            id: well
            x: parent.width * 0.5 - width / 2
            y: faderArea.capH / 2
            width: 4
            height: faderArea.travel
            radius: 2
            color: Theme.sunken
            border.color: Theme.borderSubtle
        }
        Rectangle {
            objectName: "sendWindowCap"
            width: 28
            height: faderArea.capH
            x: well.x + well.width / 2 - width / 2
            y: (1 - w.session.faderPosition(w.info.levelDb !== undefined ? w.info.levelDb : -144)) * faderArea.travel
            radius: Theme.radius
            color: Theme.hover
            border.color: Theme.borderStrong
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 } height: 1; color: Theme.highlight }
            Rectangle { anchors.centerIn: parent; width: parent.width - 6; height: 2; color: Theme.text }
        }
        MouseArea {
            objectName: "sendWindowFader"
            x: well.x - 24; width: 48 + well.width
            height: parent.height
            property real lastY: 0
            onPressed: (m) => {
                lastY = m.y
                if (m.modifiers & Qt.AltModifier) w.sends.resetLevel()
            }
            onPositionChanged: (m) => {
                if (!pressed || (m.modifiers & Qt.AltModifier)) return
                w.sends.levelDrag(m.y - lastY, faderArea.travel, (m.modifiers & Qt.ControlModifier) !== 0)
                lastY = m.y
            }
        }
    }

    Rectangle {
        id: readout
        anchors { left: parent.left; right: parent.right; bottom: remove.top; margins: Theme.space2 }
        height: 18
        radius: Theme.radiusSmall
        color: Theme.sunken
        ValueField {
            id: levelField
            objectName: "sendWindowLevel"
            anchors.fill: parent
            shell: w.shell
            text: w.info.levelText || ""
            onSubmitted: (t) => { if (w.sends.setLevelText(t)) levelField.finish() }
        }
    }
    SButton {
        id: remove
        objectName: "sendWindowRemove"
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.space2 }
        implicitHeight: 22
        bold: false
        fontPx: Theme.textSmall
        text: "Remove send"
        onClicked: w.sends.removeSend()
    }
}
