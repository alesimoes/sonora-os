// SPDX-License-Identifier: GPL-3.0-or-later
//
// Warp (ADR-0136): stretch the selected region by a ratio, along the warp
// markers placed on it, or onto a beat grid (Quantize Audio). Apply renders a
// stretched copy and the region plays it; the take is kept and one Undo goes
// back. Every action is a tempo.edit "elastic ..." text, the same words a key
// map or the Tempo dialog sends, so the panel adds no command of its own. A
// refusal stays inside the panel until the next try. On an engine built
// without the stretcher the controls are not shown at all, only why.
import QtQuick
import Sonora

Item {
    id: warpPanel
    property var shell: null
    // Whoever opens the panel fills these from the engine's Hello and the
    // selection; the markers come from the editor's marker model.
    property bool available: true
    property string unavailableReason: ""
    property string regionName: ""
    property string regionLength: ""
    property var markers: []         // [{from, to, speed}]
    property string warpList: ""     // "SRC:DST,..." for the words
    property int mode: 0             // 0 ratio, 1 markers, 2 quantize
    property int algorithm: 0
    property string ratioText: "1.5"
    property int grid: 2
    property real strength: 100
    property string problem: ""
    visible: false
    anchors.fill: parent
    z: 9

    readonly property var modes: ["Ratio", "Markers", "Quantize"]
    readonly property var algorithms: [
        { label: "Polyphonic", word: "polyphonic", detail: "Full mixes, chords, pads" },
        { label: "Rhythmic", word: "rhythmic", detail: "Drums and loops: attacks stay sharp" },
        { label: "Monophonic", word: "monophonic", detail: "One voice or line: the formant is kept" },
        { label: "Varispeed", word: "varispeed", detail: "Tape speed: pitch follows the stretch" }
    ]
    readonly property var grids: ["1/4", "1/8", "1/16", "1/32"]

    function show() {
        problem = ""
        visible = true
    }
    function close() {
        visible = false
        ratioInput.focus = false
        if (shell) shell.setTextEntry(false)
    }
    function words() {
        const algo = algorithms[algorithm].word
        if (mode === 0) return "elastic ratio " + ratioInput.text.trim() + " " + algo
        if (mode === 1) return warpList.length > 0 ? "elastic warp " + warpList + " " + algo : ""
        return "elastic quantize " + grids[grid] + " " + Math.round(strength) + " " + algo
    }
    function apply() {
        problem = ""
        const text = words()
        if (text.length === 0) {
            problem = "place a warp marker first: a warp with none moves nothing"
            return
        }
        if (!shell) return
        if (shell.invokeWith("tempo.edit", -1, text)) close()
        else problem = shell.statusText
    }

    MouseArea {
        anchors.fill: parent
        onClicked: warpPanel.close()
    }
    Rectangle {
        objectName: "warpPanel"
        z: 10
        anchors.centerIn: parent
        width: 460
        height: warpColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: warpColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space3

            Row {
                width: parent.width
                spacing: Theme.space2
                Text {
                    id: warpTitle
                    text: "Warp"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
                }
                Text {
                    anchors.baseline: warpTitle.baseline
                    text: warpPanel.regionName.length > 0
                          ? warpPanel.regionName + (warpPanel.regionLength.length > 0 ? "  ·  " + warpPanel.regionLength : "")
                          : ""
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: warpPanel.available
                      ? "Renders a stretched copy and plays it in place. The take is kept; one Undo goes back."
                      : "Elastic Audio is not in this build."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }

            // Not available: the reason, held, and nothing to press but Close.
            Rectangle {
                objectName: "warpUnavailable"
                visible: !warpPanel.available
                width: parent.width
                height: unavailableText.implicitHeight + 2 * Theme.space3
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.caution
                Text {
                    id: unavailableText
                    anchors { fill: parent; margins: Theme.space3 }
                    wrapMode: Text.Wrap
                    text: warpPanel.unavailableReason
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
            }

            // How: a ratio for the whole region, the markers, or the grid.
            Row {
                visible: warpPanel.available
                spacing: Theme.space1
                Repeater {
                    model: warpPanel.modes
                    SButton {
                        required property int index
                        required property string modelData
                        objectName: "warpMode" + modelData
                        text: modelData
                        active: index === warpPanel.mode
                        implicitHeight: 26
                        implicitWidth: 96
                        onClicked: { warpPanel.mode = index; warpPanel.problem = "" }
                    }
                }
            }

            // Ratio: new length over old.
            Row {
                visible: warpPanel.available && warpPanel.mode === 0
                width: parent.width
                spacing: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 70
                    text: "Length"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Rectangle {
                    width: 90
                    height: 26
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: ratioInput.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: ratioInput
                        objectName: "warpRatio"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        verticalAlignment: TextInput.AlignVCenter
                        text: warpPanel.ratioText
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                        onActiveFocusChanged: if (warpPanel.shell) warpPanel.shell.setTextEntry(activeFocus)
                        Keys.onEscapePressed: warpPanel.close()
                        Keys.onReturnPressed: warpPanel.apply()
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "times as long  (0.25 to 4; 2 is half speed)"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
            }

            // Markers: where each point of the region lands.
            Rectangle {
                visible: warpPanel.available && warpPanel.mode === 1
                width: parent.width
                height: Math.max(1, Math.min(warpPanel.markers.length, 6)) * 28 + 2
                radius: Theme.radiusSmall
                color: Theme.sunken
                border.color: Theme.borderSubtle
                Text {
                    visible: warpPanel.markers.length === 0
                    anchors.centerIn: parent
                    text: "No warp markers on this region"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                ListView {
                    id: markerList
                    objectName: "warpMarkers"
                    anchors { fill: parent; margins: 1 }
                    clip: true
                    model: warpPanel.markers
                    delegate: Item {
                        required property int index
                        required property var modelData
                        width: markerList.width
                        height: 28
                        Text {
                            id: markerNumber
                            anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                            width: 24
                            text: index + 1
                            color: Theme.accent
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
                        }
                        Text {
                            anchors { left: markerNumber.right; verticalCenter: parent.verticalCenter }
                            text: modelData.from + "   →   " + modelData.to
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                        }
                        Text {
                            anchors { right: parent.right; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                            text: modelData.speed
                            color: Theme.textSecondary
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                        }
                    }
                }
            }

            // Quantize: the grid the hits move to, and how far.
            Row {
                visible: warpPanel.available && warpPanel.mode === 2
                spacing: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 70
                    text: "Grid"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Repeater {
                    model: warpPanel.grids
                    SButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        active: index === warpPanel.grid
                        implicitHeight: 24
                        implicitWidth: 52
                        onClicked: warpPanel.grid = index
                    }
                }
            }
            Item {
                visible: warpPanel.available && warpPanel.mode === 2
                width: parent.width
                height: 30
                Text {
                    id: strengthLabel
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: 70
                    text: "Strength"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Item {
                    id: strengthBar
                    objectName: "warpStrength"
                    anchors { left: strengthLabel.right; right: strengthValue.left; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                    height: 22
                    readonly property real ratio: (warpPanel.strength - 1) / 99
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        height: 4
                        radius: 2
                        color: Theme.sunken
                        border.color: Theme.borderSubtle
                    }
                    Rectangle {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        width: strengthBar.ratio * strengthBar.width
                        height: 4
                        radius: 2
                        color: Theme.accent
                    }
                    Rectangle {
                        x: strengthBar.ratio * (strengthBar.width - width)
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        radius: 7
                        color: Theme.raised
                        border.color: Theme.accent
                        border.width: 2
                    }
                    MouseArea {
                        anchors.fill: parent
                        function set(x) {
                            warpPanel.strength = Math.round(1 + 99 * Math.max(0, Math.min(1, x / width)))
                        }
                        onPressed: (mouse) => set(mouse.x)
                        onPositionChanged: (mouse) => set(mouse.x)
                    }
                }
                Text {
                    id: strengthValue
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    width: 44
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(warpPanel.strength) + "%"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                }
            }

            // The algorithm: what kind of material is being stretched.
            Column {
                id: algoColumn
                visible: warpPanel.available
                width: parent.width
                spacing: 2
                Text {
                    text: "Algorithm"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
                Repeater {
                    model: warpPanel.algorithms
                    Rectangle {
                        required property int index
                        required property var modelData
                        readonly property bool chosen: index === warpPanel.algorithm
                        // A delegate has no parent while it is created.
                        width: algoColumn.width
                        height: 28
                        radius: Theme.radiusSmall
                        color: chosen ? Theme.selectionFill : (algoMouse.containsMouse ? Theme.hover : "transparent")
                        Rectangle {
                            visible: chosen
                            width: 3
                            height: parent.height
                            color: Theme.accent
                        }
                        Text {
                            anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                            text: modelData.label
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: chosen ? Font.DemiBold : Font.Normal }
                        }
                        Text {
                            anchors { right: parent.right; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                            text: modelData.detail
                            color: Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                        }
                        MouseArea {
                            id: algoMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: warpPanel.algorithm = index
                        }
                    }
                }
            }

            Text {
                objectName: "warpProblem"
                visible: warpPanel.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: warpPanel.problem
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "warpCancel"
                    text: warpPanel.available ? "Cancel" : "Close"
                    implicitHeight: 24
                    implicitWidth: 76
                    onClicked: warpPanel.close()
                }
                SButton {
                    objectName: "warpApply"
                    visible: warpPanel.available
                    text: "Apply"
                    active: true
                    implicitHeight: 24
                    implicitWidth: 76
                    onClicked: warpPanel.apply()
                }
            }
        }
    }
}
