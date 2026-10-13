// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shell: transport status, one strip per input, footer. Layout only --
// every value comes from ShellModel, and the meters are scene-graph items
// (Sonora.Meter), not anything drawn here.
//
// The visual design is ours (ADR-0004 and ADR-0006: workflow is reproduced,
// look is not).
import QtQuick
import Sonora

Rectangle {
    id: root
    required property ShellModel shell

    width: 1280
    height: 720
    color: "#0f1114"

    readonly property color ink: "#d8dde3"
    readonly property color dim: "#7d858f"
    readonly property color accent: "#4fb3a9"
    readonly property color alarm: "#ff4d4f"
    readonly property color caution: "#e0b040"
    readonly property string mono: "DejaVu Sans Mono"

    // --- transport status ---------------------------------------------------
    Rectangle {
        id: transport
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 64
        color: "#171a1f"

        Row {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            spacing: 20

            Rectangle {
                width: 84; height: 36; radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: shell.recording ? root.alarm
                     : shell.engineAlive ? "#2a2f36" : "#3a1416"
                Text {
                    anchors.centerIn: parent
                    text: shell.transportText
                    color: shell.recording ? "white" : shell.engineAlive ? root.ink : root.alarm
                    font { pixelSize: shell.engineAlive ? 18 : 13; bold: true; family: root.mono }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: shell.clockText
                color: shell.stale ? root.caution : root.ink
                font { pixelSize: 30; family: root.mono }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    // A dead engine's last numbers are not current; show none.
                    text: "take " + (shell.engineAlive ? shell.take : "--")
                    color: root.ink
                    font { pixelSize: 14; family: root.mono }
                }
                Text {
                    text: shell.formatText
                    color: root.dim
                    font { pixelSize: 12; family: root.mono }
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    text: "xruns " + (shell.engineAlive ? shell.xruns : "--")
                    color: shell.xruns > 0 ? root.alarm : root.dim
                    font { pixelSize: 12; family: root.mono }
                }
                Text {
                    text: "disk overruns " + (shell.engineAlive ? shell.overruns : "--")
                    color: shell.overruns > 0 ? root.alarm : root.dim
                    font { pixelSize: 12; family: root.mono }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: shell.healthText.length > 0
                // Stops short of the right-hand column instead of running
                // under it; the banner below carries the whole sentence.
                width: Math.max(0, rightInfo.x - 16 - x - 24)
                elide: Text.ElideRight
                text: shell.healthText
                color: root.alarm
                font { pixelSize: 14; bold: true }
            }
        }

        Column {
            id: rightInfo
            anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            Text {
                anchors.right: parent.right
                text: "keys: " + shell.profileName
                color: root.dim
                font.pixelSize: 12
            }
            Text {
                anchors.right: parent.right
                text: shell.renderPath + " · meters " + shell.meterHz + " Hz"
                color: shell.softwareRendering ? root.alarm : root.dim
                font.pixelSize: 12
            }
        }
    }

    // --- loud conditions ------------------------------------------------------
    Rectangle {
        id: banner
        anchors { left: parent.left; right: parent.right; top: transport.bottom }
        height: visible ? 26 : 0
        visible: shell.softwareRendering || !shell.engineAlive
        color: "#3a1416"
        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            color: root.alarm
            font { pixelSize: 13; bold: true }
            text: !shell.engineAlive
                  ? "The engine is not responding. Meters show no data; nothing on this screen is current."
                  : !shell.metersAvailable
                  ? "No GPU scene graph: meters cannot be drawn on this rendering path. This machine is not fit for the job (ADR-0006)."
                  : "Software rendering: no usable GPU driver. Not a supported configuration (ADR-0006); the interface costs CPU the audio needs."
        }
    }

    // --- strips -----------------------------------------------------------------
    Item {
        id: strips
        anchors {
            left: parent.left; right: parent.right
            top: banner.bottom; bottom: footer.top
            margins: 12
        }

        readonly property int scaleWidth: 34
        readonly property real stripWidth: (width - scaleWidth) / shell.trackCount
        readonly property int headerHeight: 52
        readonly property real meterHeight: height - headerHeight - 8

        // dBFS labels, placed with the meters' own mapping.
        Item {
            id: dbScale
            x: 0
            y: strips.headerHeight
            width: strips.scaleWidth - 6
            height: strips.meterHeight
            Repeater {
                model: shell.scaleMarks()
                Text {
                    required property real modelData
                    anchors.right: parent.right
                    y: shell.scaleY(modelData, dbScale.height) - height / 2
                    text: modelData === 0 ? "0" : modelData.toString()
                    color: root.dim
                    font { pixelSize: 10; family: root.mono }
                }
            }
        }

        Repeater {
            model: shell.trackCount
            Item {
                id: strip
                required property int index
                readonly property bool armed: shell.engineAlive && shell.armed[index] === true
                x: strips.scaleWidth + index * strips.stripWidth
                width: strips.stripWidth
                height: strips.height

                Text {
                    id: number
                    anchors { horizontalCenter: parent.horizontalCenter; top: parent.top }
                    text: strip.index + 1
                    color: root.ink
                    font { pixelSize: 13; family: root.mono }
                }
                // Arm. A click sends what the profile's arm key sends, through
                // the same router, so a refusal reads the same either way.
                Rectangle {
                    id: arm
                    anchors { horizontalCenter: parent.horizontalCenter; top: number.bottom; topMargin: 4 }
                    width: Math.min(28, strip.width - 10); height: 22; radius: 3
                    color: strip.armed ? root.alarm : "#252a30"
                    border.color: shell.engineAlive ? (strip.armed ? root.alarm : "#3a4048") : "#2a2d31"
                    opacity: shell.engineAlive ? 1.0 : 0.4
                    Text {
                        anchors.centerIn: parent
                        text: "R"
                        color: strip.armed ? "white" : root.dim
                        font { pixelSize: 12; bold: true }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: shell.invoke("track.toggle_arm", strip.index)
                    }
                }
                Meter {
                    objectName: "meter" + strip.index
                    source: shell
                    channel: strip.index
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: strips.headerHeight
                    width: Math.max(6, Math.min(26, strip.width - 14))
                    height: strips.meterHeight
                }
            }
        }

        // Over the meters, not instead of them: says why they are empty.
        Text {
            anchors.centerIn: parent
            visible: !shell.engineAlive || !shell.metersAvailable
            text: !shell.engineAlive ? "NO DATA" : "METERS UNAVAILABLE"
            color: root.alarm
            font { pixelSize: 40; bold: true }
            style: Text.Outline
            styleColor: "#0f1114"
        }
    }

    // --- footer -------------------------------------------------------------------
    Rectangle {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: footerColumn.implicitHeight + 16
        color: "#171a1f"

        Column {
            id: footerColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8; leftMargin: 16 }
            spacing: 4

            Flow {
                width: parent.width
                spacing: 14
                Repeater {
                    model: shell.helpEntries
                    Text {
                        required property string modelData
                        text: modelData
                        color: root.ink
                        font { pixelSize: 12; family: root.mono }
                    }
                }
            }
            Repeater {
                model: shell.notices
                Text {
                    required property string modelData
                    width: footerColumn.width
                    wrapMode: Text.Wrap
                    text: "! " + modelData
                    color: root.caution
                    font.pixelSize: 12
                }
            }
            Text {
                width: footerColumn.width
                text: shell.statusText.length > 0 ? shell.statusText : " "
                color: root.caution
                font { pixelSize: 13; bold: true }
            }
        }
    }
}
