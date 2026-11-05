// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > Bounce Mix (ui.mixer.bounce): the mix, as playback plays it, written
// to one stereo file in the session's Bounced Files folder. Offline renders
// as fast as the disk allows and runs no plug-in inserts; Real time plays the
// session and records the master, inserts included. Loudness normalise is off
// until asked. How it ended is a held notice in the footer, kept until it is
// put away here; a refusal stays in the window until the next action.
import QtQuick
import Sonora

Rectangle {
    id: win
    required property ShellModel shell
    required property SessionModel session
    property Item mover: win
    property bool realTime: false
    property bool float32: false
    property bool normalise: false
    property int targetLufs: -16

    width: 420
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.bounceOpen

    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors { fill: parent; topMargin: 4; leftMargin: 4; rightMargin: -4; bottomMargin: -4 }
        radius: Theme.radius
        color: Theme.shadow
        z: -1
    }

    component Overline: Text {
        color: Theme.textTertiary
        font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
    }

    Column {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space3 }
        spacing: Theme.space2

        Item {
            width: parent.width
            height: 22
            MouseArea {
                anchors.fill: parent
                property point grab
                onPressed: (m) => grab = Qt.point(m.x, m.y)
                onPositionChanged: (m) => {
                    const room = win.mover.parent
                    win.mover.x = Math.max(0, Math.min(room.width - win.width, win.mover.x + m.x - grab.x))
                    win.mover.y = Math.max(0, Math.min(room.height - win.height, win.mover.y + m.y - grab.y))
                }
            }
            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text: "Bounce Mix"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            SButton {
                objectName: "bounceClose"
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "×"; implicitWidth: 22; implicitHeight: 20; fontPx: Theme.textBody
                onClicked: win.shell.invoke("window.bounce", -1)
            }
        }

        Overline { text: "RANGE" }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "The whole session, from the start to the end of the last region."
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }

        Overline { text: "RENDER" }
        Row {
            spacing: Theme.space2
            SButton {
                objectName: "bounceOffline"
                text: "Offline"; implicitWidth: 120
                active: !win.realTime
                onClicked: win.realTime = false
            }
            SButton {
                objectName: "bounceRealTime"
                text: "Real time"; implicitWidth: 120
                active: win.realTime
                onClicked: win.realTime = true
            }
        }
        Text {
            objectName: "bounceModeHelp"
            width: parent.width
            wrapMode: Text.Wrap
            text: win.realTime
                  ? "Plays the session and records the master, plug-in inserts included. Takes as long as the session and needs the transport stopped."
                  : "As fast as the disk allows. Plug-in inserts are not run offline: bounce those in real time."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }

        Overline { text: "FILE" }
        Row {
            spacing: Theme.space2
            SButton {
                objectName: "bounceInt24"
                text: "24-bit"; implicitWidth: 120
                active: !win.float32
                onClicked: win.float32 = false
            }
            SButton {
                objectName: "bounceFloat32"
                text: "32-bit float"; implicitWidth: 120
                active: win.float32
                onClicked: win.float32 = true
            }
        }
        Rectangle {
            width: parent.width
            height: 24
            radius: Theme.radiusSmall
            color: Theme.window
            border.color: nameInput.activeFocus ? Theme.accent : Theme.borderStrong
            TextInput {
                id: nameInput
                objectName: "bounceName"
                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: Theme.text
                selectionColor: Theme.selectionFill
                selectedTextColor: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                onActiveFocusChanged: win.shell.setTextEntry(activeFocus)
                Keys.onEscapePressed: focus = false
                Keys.onReturnPressed: startButton.clicked(null)
                Keys.onEnterPressed: startButton.clicked(null)
                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: nameInput.text.length === 0 && !nameInput.activeFocus
                    text: "File name (default: the session's name and Bounce)"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
            }
        }

        Overline { text: "LOUDNESS" }
        Row {
            spacing: Theme.space2
            SButton {
                objectName: "bounceNormalise"
                text: "Normalise"; implicitWidth: 96
                active: win.normalise
                onClicked: win.normalise = !win.normalise
            }
            SButton {
                objectName: "bounceTargetDown"
                text: "−"; implicitWidth: 24; implicitHeight: 24
                available: win.normalise && win.targetLufs > -30
                onClicked: if (win.targetLufs > -30) win.targetLufs -= 1
            }
            Rectangle {
                width: 80; height: 24
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                opacity: win.normalise ? 1.0 : 0.45
                Text {
                    objectName: "bounceTargetValue"
                    anchors.centerIn: parent
                    text: win.targetLufs + " LUFS"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                }
            }
            SButton {
                objectName: "bounceTargetUp"
                text: "+"; implicitWidth: 24; implicitHeight: 24
                available: win.normalise && win.targetLufs < -6
                onClicked: if (win.targetLufs < -6) win.targetLufs += 1
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Off by default. On, one gain brings the whole file to the target, and never past -1 dBFS."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }

        // --- progress ---
        Rectangle {
            objectName: "bounceProgress"
            visible: win.shell.bounceState === 0
            width: parent.width
            height: 8
            radius: 4
            color: Theme.sunken
            border.color: Theme.borderSubtle
            Rectangle {
                width: Math.max(0, parent.width * win.shell.bounceProgress)
                height: parent.height
                radius: 4
                color: Theme.accent
            }
        }

        Row {
            spacing: Theme.space2
            SButton {
                id: startButton
                objectName: "bounceStart"
                text: "Bounce"; implicitWidth: 96
                available: win.shell.bounceAvailable && !win.shell.bounceRunning
                onClicked: {
                    win.shell.startBounce(win.realTime, win.float32, win.normalise,
                                          win.targetLufs, nameInput.text)
                    nameInput.focus = false
                }
            }
            SButton {
                objectName: "bounceCancel"
                text: "Cancel"; implicitWidth: 80
                available: win.shell.bounceRunning
                onClicked: win.shell.cancelBounce()
            }
            SButton {
                objectName: "bounceDismiss"
                text: "Dismiss"; implicitWidth: 80
                visible: win.shell.bounceResult.length > 0
                onClicked: win.shell.dismissBounce()
            }
        }

        // How the last one ended: also a held notice in the footer.
        Text {
            objectName: "bounceResult"
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: win.shell.bounceResult
            color: win.shell.bounceState === 2 ? Theme.caution
                 : win.shell.bounceState === 1 ? Theme.input : Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        Text {
            objectName: "bounceRefusal"
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: win.shell.bounceRefusal
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
        Text {
            width: parent.width
            visible: !win.shell.bounceAvailable
            wrapMode: Text.Wrap
            text: "This engine cannot bounce."
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
    }
}
