// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window: toolbar, rulers, track headers, and the timeline.
// The timeline and rulers are scene-graph items (Sonora.Timeline,
// Sonora.Ruler); the headers are ordinary controls, which is what they are.
// Every gesture goes to SessionModel, and every button sends the command its
// key sends; a solo button's modifier clicks (Ctrl: solo safe, Alt: every
// track) are MixModel's, the same as on a Mix window strip.
import QtQuick
import Sonora

Item {
    id: edit
    required property ShellModel shell
    required property SessionModel session
    // The solo button's modifier clicks; null where they are not wired.
    property MixModel mixer: null
    // A track header was right-clicked: the window draws the Track menu there.
    signal trackMenuRequested(real x, real y)
    // A track's playlist selector was clicked: the window draws its menu.
    signal playlistMenuRequested(int track, real x, real y)

    // A track's top in the track area, before the scroll: its lanes, when
    // disclosed, make the rows below it start lower.
    function rowTop(index) {
        const r = session.trackRows[index]
        return r !== undefined ? r.top : index * session.trackHeight
    }

    // --- toolbar --------------------------------------------------------------
    Rectangle {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.toolbarHeight
        color: Theme.panel
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }

        Row {
            id: toolRow
            anchors { left: parent.left; leftMargin: Theme.space4; verticalCenter: parent.verticalCenter }
            spacing: Theme.space4

            // Edit modes, in the order an engineer's F1-F4 reach them.
            Row {
                spacing: 2
                Repeater {
                    model: [["Shuffle", "edit.mode.shuffle"], ["Spot", "edit.mode.spot"],
                            ["Slip", "edit.mode.slip"], ["Grid", "edit.mode.grid"]]
                    SButton {
                        required property var modelData
                        text: modelData[0]
                        implicitWidth: 54
                        active: edit.session.editMode === modelData[0]
                        onClicked: edit.shell.invoke(modelData[1], -1)
                    }
                }
            }
            Row {
                spacing: 2
                Repeater {
                    model: [["Smart", "tool.smart"], ["Trim", "tool.trim"],
                            ["Select", "tool.select"], ["Grab", "tool.grab"], ["Pencil", "tool.pencil"]]
                    SButton {
                        required property var modelData
                        text: modelData[0]
                        implicitWidth: 50
                        active: edit.session.tool === modelData[0]
                        onClicked: edit.shell.invoke(modelData[1], -1)
                    }
                }
            }
            Row {
                spacing: Theme.space1
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "GRID"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; letterSpacing: 0.6 }
                }
                SButton {
                    text: edit.session.gridValue
                    implicitWidth: 52
                    bold: false
                    onClicked: edit.session.cycleGrid()
                }
            }
            // The shapes the no-dialog fade keys make, shown because a key
            // that makes an unseen shape would be guessing on the operator's
            // behalf. Laid out as the selection fields are, a caption over
            // its value, to keep the toolbar to one width at 1280 px; a
            // click on the value picks the next shape.
            Row {
                spacing: Theme.space3
                Repeater {
                    model: [["FADE", "fadeShape"], ["X-FADE", "crossfadeShape"]]
                    Column {
                        id: shapeField
                        required property var modelData
                        width: 66   // the longest shape's name, so a click moves nothing beside it
                        objectName: modelData[1]
                        Text {
                            text: shapeField.modelData[0]
                            color: Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
                        }
                        Text {
                            text: shapeField.modelData[1] === "fadeShape" ? edit.session.fadeShape
                                                                          : edit.session.crossfadeShape
                            color: shapeArea.containsMouse ? Theme.accent : Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; underline: shapeArea.containsMouse }
                            MouseArea {
                                id: shapeArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: shapeField.modelData[1] === "fadeShape"
                                           ? edit.session.cycleFadeShape()
                                           : edit.session.cycleCrossfadeShape()
                            }
                        }
                    }
                }
            }
            Row {
                spacing: 2
                SButton { text: "−"; implicitWidth: 28; fontPx: Theme.textTitle; onClicked: edit.shell.invoke("zoom.out", -1) }
                SButton { text: "+"; implicitWidth: 28; fontPx: Theme.textTitle; onClicked: edit.shell.invoke("zoom.in", -1) }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: Theme.space2
                    text: edit.session.zoomText
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                }
            }
            Row {
                spacing: Theme.space3
                Repeater {
                    model: [["START", edit.session.selectionStart], ["END", edit.session.selectionEnd],
                            ["LENGTH", edit.session.selectionLength]]
                    Column {
                        required property var modelData
                        Text {
                            text: modelData[0]
                            color: Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
                        }
                        Text {
                            text: modelData[1]
                            color: Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 } }
                        }
                    }
                }
            }
        }
        // Elided rather than drawn under the selection fields when the
        // window is narrow.
        Text {
            objectName: "sessionName"
            anchors {
                left: toolRow.right; leftMargin: Theme.space4
                right: parent.right; rightMargin: Theme.space4
                verticalCenter: parent.verticalCenter
            }
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideMiddle
            text: edit.session.sessionName
            color: edit.session.synthetic ? Theme.caution : Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
        }
    }

    // --- rulers ---------------------------------------------------------------
    Column {
        id: rulers
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom }
        Repeater {
            model: ["Timecode", "Bars|Beats", "Min:Secs"]
            Item {
                required property int index
                required property string modelData
                width: rulers.width
                height: Theme.rulerHeight
                Rectangle {
                    width: Theme.trackHeaderWidth; height: parent.height
                    color: Theme.panel
                    Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.borderSubtle }
                    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }
                    Text {
                        anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                        text: modelData
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
                    }
                }
                Ruler {
                    objectName: "ruler" + index
                    x: Theme.trackHeaderWidth
                    width: parent.width - Theme.trackHeaderWidth
                    height: parent.height
                    session: edit.session
                    format: index
                    MouseArea { anchors.fill: parent; onClicked: (m) => edit.session.rulerClick(m.x) }
                }
            }
        }
        // Tempo and Meter, from the session's tempo map: what the bars and
        // the grid above and below are read through. A click puts the
        // insertion point there, as on the time rulers; editing a change is
        // still to come (BACKLOG Track 3 item 3).
        Repeater {
            model: ["Tempo", "Meter"]
            Item {
                required property int index
                required property string modelData
                width: rulers.width
                height: Theme.rulerHeight
                Rectangle {
                    width: Theme.trackHeaderWidth; height: parent.height
                    color: Theme.panel
                    Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.borderSubtle }
                    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }
                    Text {
                        anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                        text: modelData
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
                    }
                }
                ConductorRuler {
                    objectName: index === 0 ? "tempoRuler" : "meterRuler"
                    x: Theme.trackHeaderWidth
                    width: parent.width - Theme.trackHeaderWidth
                    height: parent.height
                    session: edit.session
                    kind: index
                    MouseArea { anchors.fill: parent; onClicked: (m) => edit.session.rulerClick(m.x) }
                }
            }
        }
        // Markers: a click on one recalls it, Alt-click clears it, as
        // Option-click does in Pro Tools; "+" adds one, as Enter does.
        Item {
            width: rulers.width
            height: Theme.rulerHeight
            Rectangle {
                width: Theme.trackHeaderWidth; height: parent.height
                color: Theme.panel
                Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.borderSubtle }
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }
                SButton {
                    objectName: "markerAdd"
                    anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    text: "+"; implicitWidth: 20; implicitHeight: 16; fontPx: Theme.textSmall
                    onClicked: edit.shell.invoke("marker.add", -1)
                }
                Text {
                    anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    text: "Markers"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
                }
            }
            MarkerRuler {
                objectName: "markerRuler"
                x: Theme.trackHeaderWidth
                width: parent.width - Theme.trackHeaderWidth
                height: parent.height
                session: edit.session
                MouseArea {
                    anchors.fill: parent
                    onClicked: (m) => edit.session.markerRulerClick(m.x, (m.modifiers & Qt.AltModifier) !== 0)
                }
            }
        }
    }

    // --- tracks ---------------------------------------------------------------
    Item {
        id: tracks
        anchors { left: parent.left; right: parent.right; top: rulers.bottom; bottom: parent.bottom }
        clip: true

        // Headers scroll with the lanes, by the same offset.
        Item {
            id: headers
            width: Theme.trackHeaderWidth
            height: parent.height
            clip: true
            Repeater {
                model: edit.session.trackNames
                Rectangle {
                    id: header
                    objectName: "trackHeader" + index
                    required property int index
                    required property string modelData
                    readonly property bool selected: edit.session.selectedTracks.indexOf(index) >= 0
                    readonly property var row: edit.session.trackRows[index] || ({})
                    readonly property bool armed: edit.shell.engineAlive && edit.shell.armed[index] === true
                    y: edit.rowTop(index) - edit.session.scrollY
                    width: headers.width
                    height: edit.session.trackHeight
                    visible: y + height >= 0 && y <= headers.height
                    color: selected ? Theme.raised : Theme.panel
                    z: headerAutoMode.open ? 100 : 0

                    // Right-click, as Pro Tools' track name menu: the track
                    // is selected and the Track menu acts on it.
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            edit.session.selectTrack(header.index)
                            if (mouse.button === Qt.RightButton) {
                                const p = mapToItem(null, mouse.x, mouse.y)
                                edit.trackMenuRequested(p.x, p.y)
                            }
                        }
                        // Pro Tools: a double-click on the name renames the track.
                        onDoubleClicked: (mouse) => {
                            if (mouse.button === Qt.LeftButton &&
                                mouse.y <= headerName.y + headerName.height + Theme.space1 &&
                                mouse.x <= headerName.x + headerName.width)
                                edit.shell.invoke("track.rename", header.index)
                        }
                    }
                    // The track's colour, as its regions wear it; a click
                    // opens the palette on it.
                    Rectangle {
                        objectName: "trackColour" + header.index
                        width: 3; height: parent.height - 2
                        color: Theme.paletteColour(edit.session.trackColours[header.index] || 0)
                        MouseArea {
                            width: 10; height: parent.height
                            onClicked: {
                                edit.session.selectTrack(header.index)
                                edit.shell.invoke("track.colour", header.index)
                            }
                        }
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 1; color: Theme.borderSubtle
                    }
                    Rectangle {
                        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                        width: 1; color: Theme.borderSubtle
                    }
                    Text {
                        id: headerName
                        x: Theme.space3; y: Theme.space2
                        // Narrower where the takes button needs the room.
                        width: (parent.width - 2 * Theme.space3 - 14) * ((header.row.takes || 0) > 0 ? 0.34 : 0.45)
                        elide: Text.ElideRight
                        text: header.modelData
                        color: header.selected ? Theme.text : Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                    }
                    // The disclosure triangle: this track's automation lanes
                    // (ADR-0027), a lane for each parameter the model
                    // automates, shown under the row.
                    SButton {
                        objectName: "autolanes" + header.index
                        visible: header.row.canShow === true
                        x: parent.width - Theme.space3 - 14 - 20
                        y: Theme.space2 - 2
                        implicitWidth: 20; implicitHeight: 20
                        fontPx: Theme.textCaption
                        active: header.row.shown === true
                        text: header.row.shown === true ? "▾" : "▸"
                        onClicked: edit.session.toggleAutolanes(header.index)
                    }
                    // Pro Tools' playlist selector: the active playlist's
                    // name; a click opens the playlist menu on this track.
                    SButton {
                        id: playlistSelector
                        objectName: "playlistSelector" + header.index
                        x: headerName.x + headerName.width + Theme.space1
                        y: Theme.space2 - 2
                        width: parent.width - x - Theme.space3 - 14 - 24 -
                               ((header.row.takes || 0) > 0 ? 32 : 0)
                        implicitHeight: 20
                        bold: false
                        fontPx: Theme.textCaption
                        alignLeft: true
                        text: (edit.session.playlistNames[header.index] || "") + "  \u25BE"
                        onClicked: {
                            edit.session.selectTrack(header.index)
                            const p = mapToItem(null, 0, height)
                            edit.playlistMenuRequested(header.index, p.x, p.y)
                        }
                    }
                    // The track's other playlists, the takes of a comp: a click
                    // shows them as lanes under the track (ADR-0031, ADR-0049).
                    // Only where there are takes, so a track without any keeps
                    // its header as it was.
                    SButton {
                        objectName: "takes" + header.index
                        visible: (header.row.takes || 0) > 0
                        x: playlistSelector.x + playlistSelector.width + Theme.space1
                        y: Theme.space2 - 2
                        implicitWidth: 28; implicitHeight: 20
                        fontPx: Theme.textCaption
                        active: header.row.lanesShown === true
                        text: "\u2261 " + (header.row.takes || 0)
                        onClicked: edit.shell.invoke("comp.toggle_lanes", header.index)
                    }
                    Row {
                        x: Theme.space3
                        anchors { bottom: parent.bottom; bottomMargin: Theme.space2 }
                        spacing: Theme.space1
                        SButton {
                            text: "R"; implicitWidth: 24; implicitHeight: 20
                            objectName: "arm" + header.index
                            active: header.armed
                            activeColour: Theme.record
                            available: edit.shell.engineAlive
                            onClicked: (m) => edit.shell.armClick(header.index, m.modifiers,
                                                                  edit.session.selectedTracksFor(header.index))
                        }
                        SButton {
                            text: "I"; implicitWidth: 24; implicitHeight: 20
                            objectName: "editInput" + header.index
                            active: edit.session.inputMonitors[header.index] === true
                            activeColour: Theme.input
                            onClicked: (m) => edit.session.inputClick(header.index, m.modifiers)
                        }
                        SoloButton {
                            implicitWidth: 24; implicitHeight: 20
                            soloed: edit.session.solos[header.index] === true
                            safe: edit.session.soloSafes[header.index] === true
                            padlockName: "editSoloSafe" + header.index
                            objectName: "editSolo" + header.index
                            onClicked: (m) => {
                                if (edit.mixer === null || !edit.mixer.soloClick(header.index, m.modifiers))
                                    edit.shell.invoke("track.toggle_solo", header.index)
                            }
                        }
                        SButton {
                            text: "M"; implicitWidth: 24; implicitHeight: 20
                            active: edit.session.mutes[header.index] === true
                            activeColour: Theme.mute
                            onClicked: edit.shell.invoke("track.toggle_mute", header.index)
                        }
                        // The automation mode, as on the Mix strip, so a lane
                        // can be armed to write without leaving the Edit window.
                        AutomationModeButton {
                            id: headerAutoMode
                            objectName: "editAutoMode" + header.index
                            visible: edit.mixer !== null
                            width: 38; height: 20
                            shell: edit.shell
                            mixer: edit.mixer
                            track: header.index
                            mode: header.index < edit.session.automationModes.length
                                  ? edit.session.automationModes[header.index] : "Read"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            leftPadding: Theme.space1
                            // No unit: the automation selector took its room.
                            text: edit.session.gainText(edit.session.gains[header.index] || 0)
                            color: header.selected ? Theme.textSecondary : Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                        }
                    }
                    Meter {
                        objectName: "editMeter" + header.index
                        source: edit.shell
                        channel: header.index
                        anchors { right: parent.right; rightMargin: Theme.space2; top: parent.top; bottom: parent.bottom; margins: Theme.space1 }
                        width: 6
                    }
                }
            }
        }

        // The automation lanes under a track, one row each; the lane itself
        // is drawn by the timeline, level with this name.
        Item {
            id: laneHeaders
            width: Theme.trackHeaderWidth
            height: parent.height
            clip: true
            Repeater {
                model: edit.session.trackRows
                Item {
                    id: laneBlock
                    required property int index
                    required property var modelData
                    visible: modelData.shown === true
                    y: modelData.top + edit.session.trackHeight - edit.session.scrollY
                    width: laneHeaders.width
                    height: edit.session.autolaneNames.length * edit.session.autolaneHeight
                    Repeater {
                        model: edit.session.autolaneNames
                        Rectangle {
                            id: laneHeader
                            required property int index
                            required property string modelData
                            // Volume, Pan, Volume Trim: the timeline's colours.
                            readonly property color laneColour: index === 0 ? Theme.automationVolume
                                                              : index === 1 ? Theme.automationPan
                                                              : Theme.automationTrim
                            objectName: "autolaneHeader" + laneBlock.index + "_" + index
                            y: index * edit.session.autolaneHeight
                            width: laneBlock.width
                            height: edit.session.autolaneHeight
                            color: Theme.sunken
                            Rectangle { width: 3; height: parent.height - 2; color: laneHeader.laneColour }
                            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }
                            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.borderSubtle }
                            Text {
                                x: Theme.space3; y: Theme.space2
                                text: laneHeader.modelData
                                color: laneHeader.laneColour
                                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
                            }
                            Text {
                                x: Theme.space3; y: Theme.space2 + 16
                                width: parent.width - 2 * Theme.space3
                                elide: Text.ElideRight
                                text: (laneBlock.modelData.captions || [])[laneHeader.index] || ""
                                color: Theme.textTertiary
                                font { family: Theme.fontFamily; pixelSize: Theme.textCaption; features: { "tnum": 1 } }
                            }
                        }
                    }
                }
            }
        }

        // The take lanes under a track (ADR-0031 §1): one row for each of its
        // other playlists, the name and the button that copies the range
        // selected on the lane to the main playlist. The lane itself is
        // drawn by the timeline, level with this name.
        Item {
            id: takeHeaders
            width: Theme.trackHeaderWidth
            height: parent.height
            clip: true
            Repeater {
                model: edit.session.trackRows
                Item {
                    id: takeBlock
                    required property int index
                    required property var modelData
                    visible: modelData.lanesShown === true
                    y: modelData.top + edit.session.trackHeight + modelData.lanesTop - edit.session.scrollY
                    width: takeHeaders.width
                    height: (modelData.laneNames || []).length * edit.session.compLaneHeight
                    Repeater {
                        model: takeBlock.modelData.laneNames || []
                        Rectangle {
                            id: takeHeader
                            required property int index
                            required property string modelData
                            readonly property bool ready: edit.session.laneSelection.ready === true &&
                                edit.session.laneSelection.track === takeBlock.index &&
                                edit.session.laneSelection.lane === index
                            objectName: "takeHeader" + takeBlock.index + "_" + index
                            y: index * edit.session.compLaneHeight
                            width: takeBlock.width
                            height: edit.session.compLaneHeight
                            color: Theme.sunken
                            Rectangle { width: 3; height: parent.height - 2; color: Theme.accent }
                            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSubtle }
                            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.borderSubtle }
                            Text {
                                x: Theme.space3; y: Theme.space2
                                width: parent.width - 2 * Theme.space3 - toMain.width - Theme.space1
                                elide: Text.ElideRight
                                text: takeHeader.modelData
                                color: Theme.text
                                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
                            }
                            // Pro Tools' Copy Selection to Main Playlist. Dimmed
                            // until a range is selected here; a click then
                            // says what to select.
                            SButton {
                                id: toMain
                                objectName: "toMain" + takeBlock.index + "_" + takeHeader.index
                                x: parent.width - width - Theme.space3 - 8
                                y: Theme.space2 - 2
                                implicitWidth: 52; implicitHeight: 18
                                fontPx: Theme.textCaption
                                text: "To Main"
                                active: takeHeader.ready
                                available: takeHeader.ready
                                onClicked: edit.shell.invoke("comp.copy_to_main", -1)
                            }
                            Text {
                                x: Theme.space3; y: parent.height - height - Theme.space2
                                width: parent.width - 2 * Theme.space3 - 8
                                elide: Text.ElideRight
                                text: edit.session.quickSwipe ? "Quick Swipe: drag to comp"
                                                              : "Drag a range, then To Main"
                                color: Theme.textTertiary
                                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                            }
                        }
                    }
                }
            }
        }

        Timeline {
            id: timeline
            objectName: "timeline"
            x: Theme.trackHeaderWidth
            width: parent.width - Theme.trackHeaderWidth
            height: parent.height
            session: edit.session

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: (m) => edit.session.press(m.x, m.y)
                onPositionChanged: (m) => { if (pressed) edit.session.drag(m.x, m.y) }
                onReleased: (m) => edit.session.release(m.x, m.y)
                onDoubleClicked: (m) => edit.session.doubleClick(m.x, m.y)
                onWheel: (w) => edit.session.wheel(w.x, w.angleDelta.x / 3, w.angleDelta.y / 3,
                                                   (w.modifiers & Qt.ShiftModifier) !== 0,
                                                   (w.modifiers & Qt.ControlModifier) !== 0)
            }
        }
    }
}
