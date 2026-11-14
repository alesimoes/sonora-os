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
    property string file: "bwf"
    property int bits: 24
    property int rate: 0
    property string dither: "auto"
    property bool normalise: false
    property int targetLufs: -16

    property string stemsBy: "track"
    property var stemsOff: []   // indices of the tracks left out

    function toggleStemTrack(i) {
        const k = stemsOff.indexOf(i)
        stemsOff = k < 0 ? stemsOff.concat([i]) : stemsOff.filter(x => x !== i)
    }
    // The ids mix.stems names the checked tracks by.
    function stemsChecked() {
        const ids = []
        for (let i = 0; i < session.trackCount; ++i) {
            if (stemsOff.indexOf(i) < 0) ids.push(session.trackIdAt(i))
        }
        return ids
    }

    // FLAC and AIFF hold no 32-bit float.
    function setFile(f) {
        file = f
        if (bits === 32 && f !== "bwf" && f !== "wav") bits = 24
    }

    width: 420
    height: column.implicitHeight + 2 * Theme.space3
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong
    visible: session.bounceOpen
    // A track left out last time is not left out of the next session's export.
    onVisibleChanged: if (visible) stemsOff = []

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
            Repeater {
                model: [["bwf", "BWF"], ["wav", "WAV"], ["aiff", "AIFF"], ["flac", "FLAC"]]
                SButton {
                    required property var modelData
                    objectName: "bounceFile_" + modelData[0]
                    text: modelData[1]; implicitWidth: 90
                    active: win.file === modelData[0]
                    onClicked: win.setFile(modelData[0])
                }
            }
        }
        Row {
            spacing: Theme.space2
            SButton {
                objectName: "bounceInt16"
                text: "16-bit"; implicitWidth: 90
                active: win.bits === 16
                onClicked: win.bits = 16
            }
            SButton {
                objectName: "bounceInt24"
                text: "24-bit"; implicitWidth: 90
                active: win.bits === 24
                onClicked: win.bits = 24
            }
            SButton {
                objectName: "bounceFloat32"
                text: "32-bit float"; implicitWidth: 120
                available: win.file === "bwf" || win.file === "wav"
                active: win.bits === 32
                onClicked: win.bits = 32
            }
        }
        Text {
            objectName: "bounceFileHelp"
            width: parent.width
            wrapMode: Text.Wrap
            text: win.file === "bwf"
                  ? "Broadcast Wave: a WAV that also says where on the timeline it starts."
                  : win.file === "wav" ? "Plain WAV, no timeline origin."
                  : win.file === "aiff" ? "AIFF, 16 or 24 bits."
                  : "FLAC, lossless and about half the size; 16 or 24 bits."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }

        Overline { text: "SAMPLE RATE" }
        Row {
            spacing: Theme.space2
            Repeater {
                model: [[0, "Session"], [44100, "44.1k"], [48000, "48k"], [88200, "88.2k"], [96000, "96k"]]
                SButton {
                    required property var modelData
                    objectName: "bounceRate_" + modelData[0]
                    text: modelData[1]; implicitWidth: 72
                    active: win.rate === modelData[0]
                    onClicked: win.rate = modelData[0]
                }
            }
        }

        Overline { text: "DITHER" }
        Row {
            spacing: Theme.space2
            Repeater {
                model: [["auto", "Auto"], ["none", "Off"], ["tpdf", "TPDF"], ["shaped", "Shaped"]]
                SButton {
                    required property var modelData
                    objectName: "bounceDither_" + modelData[0]
                    text: modelData[1]; implicitWidth: 90
                    available: win.bits !== 32
                    active: win.dither === modelData[0]
                    onClicked: win.dither = modelData[0]
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Auto adds TPDF dither when the file is 16-bit. Shaped moves the noise up, out of the middle of the spectrum. 32-bit float is never dithered."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
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

        // --- Export Stems (ADR-0085): hidden unless the engine has it ---
        Overline { text: "STEMS"; visible: win.shell.stemsAvailable }
        Row {
            visible: win.shell.stemsAvailable
            spacing: Theme.space2
            SButton {
                objectName: "stemsByTrack"
                text: "By track"; implicitWidth: 120
                active: win.stemsBy === "track"
                onClicked: win.stemsBy = "track"
            }
            SButton {
                objectName: "stemsByOutput"
                text: "By output"; implicitWidth: 120
                active: win.stemsBy === "output"
                onClicked: win.stemsBy = "output"
            }
        }
        Flow {
            objectName: "stemsTracks"
            visible: win.shell.stemsAvailable && win.stemsBy === "track"
            width: parent.width
            spacing: Theme.space2
            Repeater {
                model: win.session.trackNames
                SButton {
                    required property int index
                    required property string modelData
                    objectName: "stemsTrack_" + index
                    text: modelData; implicitWidth: 96
                    active: win.stemsOff.indexOf(index) < 0
                    onClicked: win.toggleStemTrack(index)
                }
            }
        }
        Text {
            objectName: "stemsHelp"
            visible: win.shell.stemsAvailable
            width: parent.width
            wrapMode: Text.Wrap
            text: (win.stemsBy === "track"
                   ? "One file per checked track. "
                   : "One file per output the tracks feed (each bus and the master). ")
                  + "Offline only, with the file, depth, rate, dither and name above; the files go in a folder named after the name. "
                  + "Stems are not the mix: bus processing that depends on level (a compressor, say) acts on each stem alone, so their sum differs from the bounced mix."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        SButton {
            objectName: "stemsStart"
            visible: win.shell.stemsAvailable
            text: "Export Stems"; implicitWidth: 120
            available: !win.realTime && !win.shell.bounceRunning
                       && (win.stemsBy === "output" || win.stemsChecked().length > 0)
            onClicked: {
                win.shell.startStems(win.realTime, win.stemsBy,
                                     win.stemsBy === "track" ? win.stemsChecked() : [],
                                     win.file, win.bits, win.rate, win.dither, nameInput.text)
                nameInput.focus = false
            }
        }
        Text {
            objectName: "stemsRealTimeNote"
            visible: win.shell.stemsAvailable && win.realTime
            width: parent.width
            wrapMode: Text.Wrap
            text: "Stems render offline only: choose Offline above."
            color: Theme.caution
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
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
                    win.shell.startBounce(win.realTime, win.file, win.bits, win.rate,
                                          win.dither, win.normalise, win.targetLufs,
                                          nameInput.text)
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
