// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window: transport bar, loud conditions, the Edit or the Mix window,
// and the footer. Layout only -- values come from ShellModel (the engine)
// and SessionModel (the session), audio-critical drawing from the
// scene-graph items, and every colour and size from Theme
// (brain/specs/ui-design.md).
//
// The workflow is Pro Tools' and the look is ours (ADR-0004, ADR-0006).
import QtQuick
import Sonora

Rectangle {
    id: root
    required property ShellModel shell
    required property SessionModel session
    // The Mix window's insert slots; null where they are not wired.
    property InsertModel inserts: null
    // The Mix window's sends and aux buses; null where they are not wired.
    property SendModel sends: null
    // Fader rides, typed values and modifier clicks; null where not wired.
    property MixModel mixer: null

    width: 1280
    height: 720
    color: Theme.window
    // The Transport window's model; null where it is not wired, and then
    // there is no Transport window.
    property TransportModel transport: null

    TransportBar {
        id: transport
        anchors { left: parent.left; right: parent.right; top: parent.top }
        shell: root.shell
        session: root.session
        transport: root.transport
        z: 2
        onOpenRequested: openPanel.show()
        onFileMenuRequested: (x, y) => fileMenu.show(x, y)
        fileMenuOpen: fileMenu.visible
        onEditMenuRequested: (x, y) => editMenu.show(x, y)
        editMenuOpen: editMenu.visible
        onTrackMenuRequested: (x, y) => trackMenu.show(x, y)
        trackMenuOpen: trackMenu.visible
        onSetupMenuRequested: (x, y) => setupMenu.show(x, y)
        setupMenuOpen: setupMenu.visible
    }

    // --- loud conditions ------------------------------------------------------
    Rectangle {
        id: banner
        objectName: "banner"
        anchors { left: parent.left; right: parent.right; top: transport.bottom }
        // A link error can be two lines (a version refusal names both
        // versions and what to install), and it is the whole message.
        height: visible ? Math.max(26, bannerText.implicitHeight + 2 * Theme.space2) : 0
        visible: root.shell.alertText.length > 0 || root.shell.softwareRendering
                 || !root.shell.engineAlive
        color: Theme.alarmSurface
        z: 1
        Text {
            id: bannerText
            objectName: "bannerText"
            anchors {
                left: parent.left; leftMargin: Theme.space4
                right: parent.right; rightMargin: Theme.space4
                verticalCenter: parent.verticalCenter
            }
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.Bold }
            // Why the link is down comes first when it is known: a gone
            // engine's last alert is not current. Then the engine's own
            // reason: it says what to do.
            text: !root.shell.engineAlive && root.shell.linkErrorText.length > 0
                  ? root.shell.linkErrorText + ". Meters show no data; nothing on this screen is current."
                  : root.shell.alertText.length > 0
                  ? root.shell.alertText
                  : !root.shell.engineAlive
                  ? "The engine is not responding. Meters show no data; nothing on this screen is current."
                  : !root.shell.metersAvailable
                  ? "No GPU scene graph: meters and the timeline cannot be drawn on this rendering path. This machine is not fit for the job (ADR-0006)."
                  : "Software rendering: no usable GPU driver. Not a supported configuration (ADR-0006); the interface costs CPU the audio needs."
        }
    }

    Item {
        id: body
        anchors { left: parent.left; right: parent.right; top: banner.bottom; bottom: footer.top }

        EditWindow {
            objectName: "editWindow"
            anchors.fill: parent
            visible: root.session.window === "edit"
            shell: root.shell
            session: root.session
            mixer: root.mixer
            onTrackMenuRequested: (x, y) => trackMenu.show(x, y)
            onPlaylistMenuRequested: (track, x, y) => playlistMenu.show(track, x, y)
        }
        MixWindow {
            objectName: "mixWindow"
            anchors.fill: parent
            visible: root.session.window === "mix"
            shell: root.shell
            session: root.session
            inserts: root.inserts
            sends: root.sends
            mixer: root.mixer
        }

        // Over the windows, not instead of them: says why the meters are empty.
        Text {
            anchors.centerIn: parent
            visible: !root.shell.engineAlive || !root.shell.metersAvailable
            text: !root.shell.engineAlive ? "NO DATA" : "METERS UNAVAILABLE"
            color: Theme.alarm
            font { family: Theme.fontFamily; pixelSize: Theme.textDisplay; weight: Font.Bold }
            style: Text.Outline
            styleColor: Theme.window
        }
    }

    // --- window configurations: where the floating windows are ---------------
    // Each Loader below reports its place as it changes (and once, at its
    // default); a recalled configuration moves them back inside this window,
    // so one made on a larger display never leaves a window off the screen.
    function reportPlace(id, loader) { root.session.windowPlaced(id, Math.round(loader.x), Math.round(loader.y)) }
    function applyPlaces() {
        const all = { "transport": transportLoader, "undo_history": undoHistoryLoader,
                      "memory_locations": memoryLocationsLoader, "latency": latencyLoader }
        for (const id in all) {
            const p = root.session.windowPlace(id)
            if (p.x === undefined) continue
            const l = all[id]
            l.x = Math.max(0, Math.min(root.width - l.width, p.x))
            l.y = Math.max(0, Math.min(root.height - l.height, p.y))
        }
    }
    Connections {
        target: root.session
        function onWindowPlacesRecalled() { root.applyPlaces() }
    }

    // --- the Window Configurations list, floating over either window ---
    Loader {
        id: windowConfigsLoader
        objectName: "windowConfigsLoader"
        x: Math.round((root.width - width) / 2)
        y: body.y + Theme.space6
        z: 9
        sourceComponent: WindowConfigsWindow {
            objectName: "windowConfigsWindow"
            shell: root.shell
            session: root.session
            mover: windowConfigsLoader
        }
    }

    // --- the Transport window, floating over either window ---
    Loader {
        id: transportLoader
        objectName: "transportLoader"
        onXChanged: root.reportPlace("transport", transportLoader)
        onYChanged: root.reportPlace("transport", transportLoader)
        active: root.transport !== null
        // Top right of the windows to begin with; where it is dragged after.
        x: root.width - width - Theme.space6
        y: body.y + Theme.space6
        z: 8
        sourceComponent: TransportWindow {
            objectName: "transportWindow"
            shell: root.shell
            session: root.session
            transport: root.transport
            mover: transportLoader
        }
    }

    // --- the Undo History window, floating over either window ---
    Loader {
        id: undoHistoryLoader
        objectName: "undoHistoryLoader"
        onXChanged: root.reportPlace("undo_history", undoHistoryLoader)
        onYChanged: root.reportPlace("undo_history", undoHistoryLoader)
        // Below the Transport window's place, at the left of the timeline.
        x: Theme.trackHeaderWidth + Theme.space6
        y: body.y + Theme.space6
        z: 8
        sourceComponent: UndoHistoryWindow {
            objectName: "undoHistoryWindow"
            shell: root.shell
            session: root.session
            mover: undoHistoryLoader
        }
    }

    // --- the Memory Locations window, floating over either window ---
    Loader {
        id: memoryLocationsLoader
        objectName: "memoryLocationsLoader"
        onXChanged: root.reportPlace("memory_locations", memoryLocationsLoader)
        onYChanged: root.reportPlace("memory_locations", memoryLocationsLoader)
        // At the right, under the Transport window's place, so it leaves the
        // rulers and the Undo History window's place in view.
        x: root.width - width - Theme.space6
        y: body.y + 200
        z: 8
        sourceComponent: MemoryLocationsWindow {
            objectName: "memoryLocationsWindow"
            shell: root.shell
            session: root.session
            mover: memoryLocationsLoader
        }
    }

    // --- the Latency window (Setup > Latency…), floating over either window ---
    Loader {
        id: latencyLoader
        objectName: "latencyLoader"
        onXChanged: root.reportPlace("latency", latencyLoader)
        onYChanged: root.reportPlace("latency", latencyLoader)
        x: root.width - width - Theme.space6
        y: body.y + Theme.space6
        z: 8
        sourceComponent: LatencyWindow {
            objectName: "latencyWindow"
            shell: root.shell
            session: root.session
            mover: latencyLoader
        }
    }

    // --- footer -------------------------------------------------------------------
    Rectangle {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: footerColumn.implicitHeight + 2 * Theme.space2
        color: Theme.panel
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: Theme.borderSubtle }

        Column {
            id: footerColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space2; leftMargin: Theme.space4; rightMargin: Theme.space4 }
            spacing: 2

            Text {
                width: footerColumn.width
                text: root.shell.statusText.length > 0 ? root.shell.statusText : " "
                color: Theme.caution
                elide: Text.ElideRight
                font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
            }
            // One per value the loader changed to open this session, kept
            // until dismissed: the old and the new value under its key path.
            Repeater {
                model: root.session.repairs
                Row {
                    id: repairRow
                    objectName: "repairNotice"
                    required property var modelData
                    required property int index
                    width: footerColumn.width
                    spacing: Theme.space2
                    SButton {
                        objectName: "dismissRepair"
                        text: "Dismiss"
                        implicitHeight: 18
                        fontPx: Theme.textCaption
                        onClicked: root.session.dismissRepair(repairRow.index)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - parent.spacing - parent.children[0].width
                        wrapMode: Text.Wrap
                        text: "Repaired to open: " + repairRow.modelData.path + "  " +
                              repairRow.modelData.was + " → " + repairRow.modelData.now +
                              "  (" + repairRow.modelData.why + ")"
                        color: Theme.caution
                        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                    }
                }
            }
            // A track command refused during a take (ADR-0023 §5), kept
            // until dismissed or until a track command goes through.
            Row {
                objectName: "trackRefusal"
                visible: root.session.trackRefusal.length > 0
                width: footerColumn.width
                spacing: Theme.space2
                SButton {
                    id: dismissTrackRefusal
                    objectName: "dismissTrackRefusal"
                    text: "Dismiss"
                    implicitHeight: 18
                    fontPx: Theme.textCaption
                    onClicked: root.session.dismissTrackRefusal()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - parent.spacing - dismissTrackRefusal.width
                    wrapMode: Text.Wrap
                    text: "! " + root.session.trackRefusal
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
            // An output change refused (ADR-0024), kept until dismissed or
            // until a routing change goes through.
            Row {
                objectName: "routingRefusal"
                visible: root.session.routingRefusal.length > 0
                width: footerColumn.width
                spacing: Theme.space2
                SButton {
                    id: dismissRoutingRefusal
                    objectName: "dismissRoutingRefusal"
                    text: "Dismiss"
                    implicitHeight: 18
                    fontPx: Theme.textCaption
                    onClicked: root.session.dismissRoutingRefusal()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - parent.spacing - dismissRoutingRefusal.width
                    wrapMode: Text.Wrap
                    text: "! " + root.session.routingRefusal
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
            Repeater {
                model: root.shell.notices
                Text {
                    required property string modelData
                    width: footerColumn.width
                    wrapMode: Text.Wrap
                    text: "! " + modelData
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
            // The profile (it opens the Keys menu), Pro Tools' a-z, lit
            // while Commands Keyboard Focus is on, and the keys this build
            // answers on one line; the profile has more.
            Row {
                width: footerColumn.width
                spacing: Theme.space2
                SButton {
                    id: keysButton
                    objectName: "keysButton"
                    text: "Keys: " + root.shell.profileName
                    implicitHeight: 18
                    fontPx: Theme.textCaption
                    active: profilePanel.visible
                    onClicked: profilePanel.visible = !profilePanel.visible
                }
                SButton {
                    id: focusButton
                    objectName: "focusButton"
                    visible: root.shell.focusKeys
                    text: "a-z"
                    implicitHeight: 18
                    fontPx: Theme.textCaption
                    active: root.shell.commandsFocus
                    onClicked: root.shell.invoke("focus.commands", -1)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - keysButton.width - (focusButton.visible ? focusButton.width + parent.spacing : 0) - parent.spacing
                    elide: Text.ElideRight
                    text: root.shell.helpEntries.join("   ")
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
            }
        }
    }

    // --- Open: the sessions folder, and nothing else ----------------------------
    // The appliance has no file browser (an operator must never reach one);
    // the panel lists the session folders File > Save makes, by name.
    MouseArea {
        anchors.fill: parent
        visible: openPanel.visible
        z: 9
        onClicked: openPanel.visible = false
    }
    Rectangle {
        id: openPanel
        objectName: "openPanel"
        property var entries: []
        property var recordings: []
        property bool discard: false
        function show() {
            entries = root.session.sessionList()
            recordings = root.session.unsavedRecordingList()
            discard = false
            visible = true
        }
        visible: false
        z: 10
        anchors.centerIn: parent
        width: 520
        height: panelColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click inside does not close it

        Column {
            id: panelColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Open session"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                elide: Text.ElideMiddle
                text: root.session.sessionsFolder()
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Text {
                visible: openPanel.entries.length === 0
                text: "No saved sessions yet. Save puts one here."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
            Repeater {
                model: openPanel.entries
                SButton {
                    required property var modelData
                    width: panelColumn.width
                    implicitHeight: 28
                    bold: false
                    fontPx: Theme.textBody
                    text: modelData.name + (modelData.current ? "  (open now)" : "")
                    onClicked: if (root.session.openSession(modelData.file, openPanel.discard)) openPanel.visible = false
                }
            }
            // ADR-0018 §3: takes the engine was recording into a session
            // never saved; opening one offers them for placing.
            Text {
                visible: openPanel.recordings.length > 0
                topPadding: Theme.space2
                text: "Unsaved recordings"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
            }
            Text {
                visible: openPanel.recordings.length > 0
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Takes with no session file. Open one to place its takes, then save to name it."
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Repeater {
                model: openPanel.recordings
                SButton {
                    required property var modelData
                    objectName: "unsavedRecording"
                    width: panelColumn.width
                    implicitHeight: 28
                    bold: false
                    fontPx: Theme.textBody
                    text: modelData.name + (modelData.current ? "  (open now)" : "")
                    onClicked: if (root.session.openRecording(modelData.folder, openPanel.discard)) openPanel.visible = false
                }
            }
            // Unsaved edits are lost only when the operator says so.
            Row {
                visible: root.session.dirty
                spacing: Theme.space2
                SButton {
                    text: "Discard edits"
                    implicitHeight: 24
                    active: openPanel.discard
                    activeColour: Theme.caution
                    onClicked: openPanel.discard = !openPanel.discard
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "This session has unsaved edits."
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
            Row {
                spacing: Theme.space2
                SButton {
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: openPanel.visible = false
                }
                // Pro Tools' "Create From Template", beside Open.
                SButton {
                    objectName: "openNewFromTemplate"
                    text: "New from Template…"
                    implicitHeight: 24
                    onClicked: {
                        openPanel.visible = false
                        templatePanel.show()
                    }
                }
            }
        }
    }

    // --- Recovered takes: on disk, not in this session --------------------------
    // The engine never places them on its own (session-format.md, "Crash
    // recovery"): the operator answers, and until then the offer stays. A
    // click outside does not close it; leaving them keeps the files, and
    // the next open offers them again.
    MouseArea {
        anchors.fill: parent
        visible: recoveryPanel.visible
        z: 11
    }
    Rectangle {
        id: recoveryPanel
        objectName: "recoveryPanel"
        visible: root.shell.recoveryOffer.length > 0
        z: 12
        anchors.centerIn: parent
        width: Math.min(720, root.width - 2 * Theme.space6)
        height: recoveryColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.caution

        Column {
            id: recoveryColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: root.shell.recoveryCount === 1 ? "Recovered take" : "Recovered takes"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: (root.shell.recoveryCount === 1 ? "One take is" : root.shell.recoveryCount + " take files are") +
                      " in Audio Files but not in this session: recorded after the last save, or" +
                      " while the engine stopped. Each header says where it was recorded."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
            Rectangle {
                width: parent.width
                height: offerLines.implicitHeight + 2 * Theme.space2
                radius: Theme.radiusSmall
                color: Theme.sunken
                Column {
                    id: offerLines
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space2 }
                    spacing: 2
                    Repeater {
                        model: root.shell.recoveryOffer
                        Text {
                            objectName: "recoveryLine"
                            required property string modelData
                            width: offerLines.width
                            elide: Text.ElideRight
                            text: modelData
                            color: modelData.indexOf("audio pool only") >= 0 ? Theme.textTertiary : Theme.text
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                        }
                    }
                }
            }
            Text {
                objectName: "recoveryRefusal"
                visible: root.shell.recoveryRefusal.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: root.shell.recoveryRefusal
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold }
            }
            Row {
                spacing: Theme.space2
                SButton {
                    objectName: "placeRecovered"
                    text: "Place on Timeline"
                    implicitHeight: 28
                    implicitWidth: 160
                    active: true
                    onClicked: root.shell.invoke("session.place_recovered", -1)
                }
                SButton {
                    objectName: "leaveRecovered"
                    text: "Leave in Audio Files"
                    implicitHeight: 28
                    implicitWidth: 160
                    onClicked: root.shell.invoke("session.leave_recovered", -1)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Placing is one undo step per take."
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
            }
        }
    }

    // --- the File menu ---------------------------------------------------------
    // Save, Save As and Save Copy In send what their keys send; Open is the
    // panel the Open button shows.
    MouseArea {
        anchors.fill: parent
        visible: fileMenu.visible
        z: 9
        onClicked: fileMenu.visible = false
    }
    Rectangle {
        id: fileMenu
        objectName: "fileMenu"
        visible: false
        z: 10
        function show(px, py) {
            x = Math.min(px, root.width - width - Theme.space2)
            y = py + Theme.space1
            visible = true
        }
        width: 220
        height: fileColumn.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: fileColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space1 }
            spacing: 2
            Repeater {
                model: [
                    { name: "fileSave", label: "Save", command: "app.save" },
                    { name: "fileSaveAs", label: "Save As…", command: "app.save_as" },
                    { name: "fileSaveCopyIn", label: "Save Copy In…", command: "app.save_copy_in" },
                    { name: "fileSaveAsTemplate", label: "Save as Template…", command: "app.save_as_template" },
                    { name: "", label: "", command: "" },
                    { name: "fileOpen", label: "Open…", command: "open" },
                    { name: "fileNewFromTemplate", label: "New from Template…", command: "app.new_from_template" }
                ]
                Item {
                    required property var modelData
                    width: fileColumn.width
                    height: modelData.command.length > 0 ? 26 : 7
                    Rectangle {
                        visible: modelData.command.length === 0
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        height: 1
                        color: Theme.borderSubtle
                    }
                    SButton {
                        objectName: modelData.name
                        visible: modelData.command.length > 0
                        anchors.fill: parent
                        bold: false
                        fontPx: Theme.textBody
                        text: modelData.label
                        onClicked: {
                            fileMenu.visible = false
                            if (modelData.command === "open") openPanel.show()
                            else root.shell.invoke(modelData.command, -1)
                        }
                    }
                }
            }
        }
    }

    // --- Save As / Save Copy In: the new session's name ------------------------
    // Pro Tools' dialogs, asked for by app.save_as and app.save_copy_in with
    // no name. OK and Return send the command with the name through the same
    // router; a refused name keeps the dialog open with the reason in it,
    // held until it is corrected, not only on the status line.
    MouseArea {
        anchors.fill: parent
        visible: saveAsPanel.visible
        z: 9
        onClicked: saveAsPanel.close()
    }
    Rectangle {
        id: saveAsPanel
        objectName: "saveAsPanel"
        property bool copy: false
        // File > Save as Template: the same dialog, named for a template.
        property bool asTemplate: false
        property string where
        property string problem
        visible: false
        z: 10
        anchors.centerIn: parent
        width: 400
        height: saveAsColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        function show(copy, name, where) {
            saveAsPanel.copy = copy
            saveAsPanel.asTemplate = false
            saveAsPanel.where = where
            saveAsPanel.problem = ""
            saveAsInput.text = name
            visible = true
            saveAsInput.forceActiveFocus()
            saveAsInput.selectAll()
        }
        function showTemplate(name, where) {
            show(false, name, where)
            saveAsPanel.asTemplate = true
        }
        function close() {
            visible = false
            saveAsInput.focus = false
            root.shell.setTextEntry(false)
        }
        function accept() {
            const command = saveAsPanel.asTemplate ? "app.save_as_template"
                          : saveAsPanel.copy ? "app.save_copy_in" : "app.save_as"
            if (root.shell.invokeWith(command, -1, saveAsInput.text)) close()
            else saveAsPanel.problem = root.shell.statusText
        }

        Column {
            id: saveAsColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: saveAsPanel.asTemplate ? "Save as Template" : saveAsPanel.copy ? "Save Copy In" : "Save As"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                objectName: "saveAsWhat"
                width: parent.width
                wrapMode: Text.Wrap
                text: (saveAsPanel.asTemplate
                       ? "This session's tracks, routing, inserts and mix, without its audio or regions. You stay in this session."
                       : saveAsPanel.copy
                       ? "A copy of this session in a new folder. You stay in this session."
                       : "This session in a new folder, with its audio. You go on in the new one; this one stays as it is.")
                      + " In " + saveAsPanel.where + "."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                spacing: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: saveAsPanel.asTemplate ? "Template name" : "Session name"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Rectangle {
                    width: 240
                    height: 26
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: saveAsInput.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: saveAsInput
                        objectName: "saveAsInput"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        selectedTextColor: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                        onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                        onTextChanged: saveAsPanel.problem = ""
                        Keys.onEscapePressed: saveAsPanel.close()
                        Keys.onReturnPressed: saveAsPanel.accept()
                        Keys.onEnterPressed: saveAsPanel.accept()
                    }
                }
            }
            // Pro Tools' "Items To Copy: All audio files". Without it the
            // copy's audio is offline until copied in by hand.
            SButton {
                objectName: "saveAsAudio"
                visible: saveAsPanel.copy
                text: (root.session.copyWithAudio ? "☑" : "☐") + "  All audio files"
                bold: false
                implicitHeight: 24
                onClicked: root.session.copyWithAudio = !root.session.copyWithAudio
            }
            // ADR-0030 §2: a template drops its markers unless asked.
            SButton {
                objectName: "saveAsTemplateMarkers"
                visible: saveAsPanel.asTemplate
                text: (root.session.templateMarkers ? "☑" : "☐") + "  Markers"
                bold: false
                implicitHeight: 24
                onClicked: root.session.templateMarkers = !root.session.templateMarkers
            }
            Text {
                objectName: "saveAsProblem"
                visible: saveAsPanel.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: saveAsPanel.problem
                color: Theme.alarm
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "saveAsCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: saveAsPanel.close()
                }
                SButton {
                    objectName: "saveAsOk"
                    text: "Save"
                    implicitHeight: 24
                    active: true
                    onClicked: saveAsPanel.accept()
                }
            }
        }
    }

    // --- New from Template: the templates folder, and nothing else -------------
    // Pro Tools' "Create From Template", as a list like Open's (ADR-0030):
    // each template with its date and track count; one New from Template
    // would refuse is shown with the reason and cannot be chosen. Rename and
    // Delete act on the row; Delete asks first, on the row itself.
    MouseArea {
        anchors.fill: parent
        visible: templatePanel.visible
        z: 9
        onClicked: templatePanel.close()
    }
    Rectangle {
        id: templatePanel
        objectName: "templatePanel"
        property var entries: []
        property string chosen        // the template's file
        property string chosenName
        property string confirming    // the template Delete asks about
        property string renaming      // the template whose name is edited
        property string problem
        property bool discard: false
        visible: false
        z: 10
        anchors.centerIn: parent
        width: 600
        height: templateColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        function refresh() {
            entries = root.session.templateList()
            let still = false
            for (let i = 0; i < entries.length; ++i) {
                if (entries[i].file === chosen && entries[i].problem.length === 0) still = true
            }
            if (!still) {
                chosen = ""
                chosenName = ""
                for (let i = 0; i < entries.length; ++i) {
                    if (entries[i].problem.length === 0) {
                        choose(entries[i])
                        break
                    }
                }
            }
        }
        function show() {
            chosen = ""
            confirming = ""
            renaming = ""
            problem = ""
            discard = false
            refresh()
            visible = true
        }
        function choose(entry) {
            chosen = entry.file
            chosenName = entry.name
            templateNameInput.text = root.session.unusedSessionName(entry.name)
            problem = ""
        }
        function close() {
            visible = false
            templateNameInput.focus = false
            renaming = ""
            root.shell.setTextEntry(false)
        }
        function accept() {
            if (chosen.length === 0) return
            if (root.session.newFromTemplate(chosen, templateNameInput.text, discard)) close()
            else problem = root.shell.statusText
        }
        function rename(to) {
            if (root.session.renameTemplate(renaming, to)) {
                renaming = ""
                root.shell.setTextEntry(false)
                refresh()
            } else {
                problem = root.shell.statusText
            }
        }
        function remove(name) {
            confirming = ""
            if (!root.session.deleteTemplate(name)) problem = root.shell.statusText
            refresh()
        }

        Column {
            id: templateColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "New from Template"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                elide: Text.ElideMiddle
                text: root.session.templatesFolder()
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Text {
                visible: templatePanel.entries.length === 0
                width: parent.width
                wrapMode: Text.Wrap
                text: "No templates yet. File > Save as Template puts one here."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
            Repeater {
                model: templatePanel.entries
                Rectangle {
                    id: templateRow
                    required property var modelData
                    readonly property bool usable: modelData.problem.length === 0
                    readonly property bool picked: modelData.file === templatePanel.chosen
                    readonly property bool asking: modelData.name === templatePanel.confirming
                    readonly property bool editing: modelData.name === templatePanel.renaming
                    objectName: "templateRow"
                    width: templateColumn.width
                    height: 44
                    radius: Theme.radiusSmall
                    color: picked ? Theme.selectionFill : rowArea.containsMouse && usable ? Theme.hover : Theme.sunken
                    border.color: picked ? Theme.accent : asking ? Theme.alarm : "transparent"

                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: templateRow.usable && !templateRow.editing
                        onClicked: templatePanel.choose(templateRow.modelData)
                        onDoubleClicked: {
                            templatePanel.choose(templateRow.modelData)
                            templatePanel.accept()
                        }
                    }
                    Column {
                        anchors { left: parent.left; leftMargin: Theme.space3; verticalCenter: parent.verticalCenter
                                  right: rowButtons.left; rightMargin: Theme.space2 }
                        spacing: 2
                        Text {
                            visible: !templateRow.editing
                            width: parent.width
                            elide: Text.ElideRight
                            text: templateRow.modelData.name
                            color: templateRow.usable ? Theme.text : Theme.textTertiary
                            font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                        }
                        Rectangle {
                            visible: templateRow.editing
                            width: parent.width
                            height: 22
                            radius: Theme.radiusSmall
                            color: Theme.window
                            border.color: Theme.accent
                            TextInput {
                                id: renameInput
                                objectName: "templateRenameInput"
                                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                                verticalAlignment: TextInput.AlignVCenter
                                clip: true
                                color: Theme.text
                                selectionColor: Theme.selectionFill
                                selectedTextColor: Theme.text
                                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                                onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                                onTextChanged: templatePanel.problem = ""
                                Keys.onEscapePressed: { templatePanel.renaming = ""; focus = false }
                                Keys.onReturnPressed: templatePanel.rename(text)
                                Keys.onEnterPressed: templatePanel.rename(text)
                            }
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: templateRow.asking
                                  ? "Delete it and its plugin states? Sessions made from it keep theirs."
                                  : templateRow.usable
                                  ? (templateRow.modelData.tracks === 1 ? "1 track" : templateRow.modelData.tracks + " tracks")
                                    + "  ·  saved " + templateRow.modelData.date
                                  : "Cannot be used: " + templateRow.modelData.problem
                            color: templateRow.asking ? Theme.alarm : templateRow.usable ? Theme.textSecondary : Theme.caution
                            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                        }
                    }
                    Row {
                        id: rowButtons
                        anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                        spacing: Theme.space1
                        SButton {
                            visible: !templateRow.asking && !templateRow.editing
                            objectName: "templateRename"
                            text: "Rename…"
                            bold: false
                            implicitHeight: 24
                            onClicked: {
                                templatePanel.confirming = ""
                                templatePanel.problem = ""
                                templatePanel.renaming = templateRow.modelData.name
                                renameInput.text = templateRow.modelData.name
                                renameInput.forceActiveFocus()
                                renameInput.selectAll()
                            }
                        }
                        SButton {
                            visible: !templateRow.asking && !templateRow.editing
                            objectName: "templateDelete"
                            text: "Delete…"
                            bold: false
                            implicitHeight: 24
                            onClicked: {
                                templatePanel.renaming = ""
                                templatePanel.problem = ""
                                templatePanel.confirming = templateRow.modelData.name
                            }
                        }
                        SButton {
                            visible: templateRow.asking
                            text: "Keep"
                            implicitHeight: 24
                            onClicked: templatePanel.confirming = ""
                        }
                        SButton {
                            visible: templateRow.asking
                            objectName: "templateDeleteConfirm"
                            text: "Delete"
                            implicitHeight: 24
                            active: true
                            activeColour: Theme.alarm
                            onClicked: templatePanel.remove(templateRow.modelData.name)
                        }
                        SButton {
                            visible: templateRow.editing
                            text: "Cancel"
                            implicitHeight: 24
                            onClicked: {
                                templatePanel.renaming = ""
                                renameInput.focus = false
                            }
                        }
                        SButton {
                            visible: templateRow.editing
                            text: "Rename"
                            implicitHeight: 24
                            active: true
                            onClicked: templatePanel.rename(renameInput.text)
                        }
                    }
                }
            }
            Row {
                visible: templatePanel.entries.length > 0
                spacing: Theme.space2
                topPadding: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "New session name"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Rectangle {
                    width: 280
                    height: 26
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: templateNameInput.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: templateNameInput
                        objectName: "templateNameInput"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        selectedTextColor: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                        onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                        onTextChanged: templatePanel.problem = ""
                        Keys.onEscapePressed: templatePanel.close()
                        Keys.onReturnPressed: templatePanel.accept()
                        Keys.onEnterPressed: templatePanel.accept()
                    }
                }
            }
            Text {
                visible: templatePanel.entries.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: "In " + root.session.sessionsFolder() + ". Record works once the new session is saved there."
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Row {
                visible: root.session.dirty
                spacing: Theme.space2
                SButton {
                    text: "Discard edits"
                    implicitHeight: 24
                    active: templatePanel.discard
                    activeColour: Theme.caution
                    onClicked: templatePanel.discard = !templatePanel.discard
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "This session has unsaved edits."
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
                }
            }
            Text {
                objectName: "templateProblem"
                visible: templatePanel.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: templatePanel.problem
                color: Theme.alarm
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "templateCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: templatePanel.close()
                }
                SButton {
                    objectName: "templateCreate"
                    text: "Create Session"
                    implicitHeight: 24
                    implicitWidth: 130
                    active: templatePanel.chosen.length > 0
                    available: templatePanel.chosen.length > 0
                    onClicked: templatePanel.accept()
                }
            }
        }
    }

    // --- the Edit menu ---------------------------------------------------------
    // Every item sends the command its key sends, through the same router, so
    // the menu and the keyboard cannot disagree. An item with nothing to act
    // on is dimmed and still answers, on the status line, with the reason.
    MouseArea {
        anchors.fill: parent
        visible: editMenu.visible
        z: 9
        onClicked: editMenu.visible = false
    }
    Rectangle {
        id: editMenu
        objectName: "editMenu"
        visible: false
        z: 10
        function show(px, py) {
            x = Math.min(px, root.width - width - Theme.space2)
            y = py + Theme.space1
            visible = true
        }
        width: 220
        height: editColumn.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click between items does not fall through

        Column {
            id: editColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space1 }
            spacing: 2
            Repeater {
                model: [
                    { name: "editUndo", label: root.session.undoText, command: "edit.undo", on: root.session.canUndo },
                    { name: "editRedo", label: root.session.redoText, command: "edit.redo", on: root.session.canRedo },
                    { name: "editUndoHistory", label: "Undo History", command: "window.undo_history", on: true },
                    { name: "", label: "", command: "", on: false },
                    { name: "editCut", label: "Cut", command: "edit.cut", on: true },
                    { name: "editCopy", label: "Copy", command: "edit.copy", on: true },
                    { name: "editPaste", label: "Paste", command: "edit.paste", on: root.session.canPaste },
                    { name: "editClear", label: "Clear", command: "edit.clear", on: true },
                    { name: "editDuplicate", label: "Duplicate", command: "edit.duplicate", on: true },
                    { name: "editRepeat", label: "Repeat…", command: "edit.repeat", on: true },
                    { name: "", label: "", command: "", on: false },
                    { name: "editRenameRegion", label: "Rename Region…", command: "region.rename", on: true },
                    { name: "editRegionColour", label: "Region Colour…", command: "region.colour", on: true },
                    { name: "", label: "", command: "", on: false },
                    { name: "editSelectAll", label: "Select All", command: "edit.select_all", on: true },
                    { name: "", label: "", command: "", on: false },
                    { name: "editNewMarker", label: "New Marker…", command: "marker.add", on: true },
                    { name: "editMemoryLocations", label: "Memory Locations", command: "window.memory_locations", on: true }
                ]
                Item {
                    required property var modelData
                    width: editColumn.width
                    height: modelData.command.length > 0 ? 26 : 7
                    Rectangle {
                        visible: modelData.command.length === 0
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        height: 1
                        color: Theme.borderSubtle
                    }
                    SButton {
                        objectName: modelData.name
                        visible: modelData.command.length > 0
                        anchors.fill: parent
                        bold: false
                        fontPx: Theme.textBody
                        available: modelData.on
                        text: modelData.label
                        onClicked: {
                            editMenu.visible = false
                            root.shell.invoke(modelData.command, -1)
                        }
                    }
                }
            }
        }
    }

    // --- the Track menu ---------------------------------------------------------
    // Pro Tools' Track menu (ADR-0023), on the selected track: each item sends
    // the command its key sends.
    MouseArea {
        anchors.fill: parent
        visible: trackMenu.visible
        z: 9
        onClicked: trackMenu.visible = false
    }
    Rectangle {
        id: trackMenu
        objectName: "trackMenu"
        visible: false
        z: 10
        function show(px, py) {
            x = Math.min(px, root.width - width - Theme.space2)
            y = Math.min(py + Theme.space1, root.height - height - Theme.space2)
            visible = true
        }
        width: 220
        height: trackColumn.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: trackColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space1 }
            spacing: 2
            Repeater {
                model: {
                    const sel = root.session.selectedTrack
                    const n = root.session.trackCount
                    return [
                        { name: "trackNew", label: "New…", command: "track.new", on: true },
                        { name: "trackDuplicate", label: "Duplicate", command: "track.duplicate", on: sel >= 0 },
                        { name: "trackDelete", label: "Delete", command: "track.delete", on: sel >= 0 },
                        { name: "", label: "", command: "", on: false },
                        { name: "trackRename", label: "Rename…", command: "track.rename", on: sel >= 0 },
                        { name: "trackColourItem", label: "Colour…", command: "track.colour", on: sel >= 0 },
                        { name: "", label: "", command: "", on: false },
                        { name: "trackMoveUp", label: "Move Up", command: "track.move_up", on: sel > 0 },
                        { name: "trackMoveDown", label: "Move Down", command: "track.move_down", on: sel >= 0 && sel < n - 1 }
                    ]
                }
                Item {
                    required property var modelData
                    width: trackColumn.width
                    height: modelData.command.length > 0 ? 26 : 7
                    Rectangle {
                        visible: modelData.command.length === 0
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        height: 1
                        color: Theme.borderSubtle
                    }
                    SButton {
                        objectName: modelData.name
                        visible: modelData.command.length > 0
                        anchors.fill: parent
                        bold: false
                        fontPx: Theme.textBody
                        available: modelData.on
                        text: modelData.label
                        onClicked: {
                            trackMenu.visible = false
                            root.shell.invoke(modelData.command, -1)
                        }
                    }
                }
            }
        }
    }

    // --- New Tracks: how many ------------------------------------------------------
    // Pro Tools' New Tracks dialog, asked for by track.new with no count:
    // mono audio tracks, below the selected one. Create and Return send
    // track.new with the number; a refusal keeps the dialog open.
    MouseArea {
        anchors.fill: parent
        visible: newTracksPanel.visible
        z: 9
        onClicked: newTracksPanel.close()
    }
    Rectangle {
        id: newTracksPanel
        objectName: "newTracksPanel"
        visible: false
        z: 10
        anchors.centerIn: parent
        width: 340
        height: newTracksColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        function show() {
            newTracksInput.text = "1"
            visible = true
            newTracksInput.forceActiveFocus()
            newTracksInput.selectAll()
        }
        function close() {
            visible = false
            newTracksInput.focus = false
            root.shell.setTextEntry(false)
        }
        function accept() {
            if (root.shell.invokeWith("track.new", -1, newTracksInput.text)) close()
        }

        Column {
            id: newTracksColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "New Tracks"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Mono audio tracks, below the selected track, each on the next free input."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                spacing: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Number of tracks"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Rectangle {
                    width: 72
                    height: 26
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: newTracksInput.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: newTracksInput
                        objectName: "newTracksInput"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        horizontalAlignment: TextInput.AlignRight
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        selectedTextColor: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                        onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                        Keys.onEscapePressed: newTracksPanel.close()
                        Keys.onReturnPressed: newTracksPanel.accept()
                        Keys.onEnterPressed: newTracksPanel.accept()
                    }
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "newTracksCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: newTracksPanel.close()
                }
                SButton {
                    objectName: "newTracksCreate"
                    text: "Create"
                    implicitHeight: 24
                    onClicked: newTracksPanel.accept()
                }
            }
        }
    }

    // --- the playlist menu --------------------------------------------------------
    // Pro Tools' playlist selector (ADR-0026), from a track header: New,
    // Duplicate, Rename and Delete Unused, then the track's playlists, the
    // active one ticked. Each item sends the command its key sends.
    MouseArea {
        anchors.fill: parent
        visible: playlistMenu.visible
        z: 9
        onClicked: playlistMenu.visible = false
    }
    Rectangle {
        id: playlistMenu
        objectName: "playlistMenu"
        visible: false
        z: 10
        property int track: -1
        property var names: []
        function show(t, px, py) {
            track = t
            names = root.session.trackPlaylists(t)
            x = Math.min(px, root.width - width - Theme.space2)
            y = Math.min(py + Theme.space1, root.height - height - Theme.space2)
            visible = true
        }
        width: 240
        height: playlistColumn.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: playlistColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space1 }
            spacing: 2
            Repeater {
                model: {
                    const active = root.session.playlistNames[playlistMenu.track] || ""
                    const items = [
                        { name: "playlistNew", label: "New", command: "track.playlist_new", argument: "", on: true },
                        { name: "playlistDuplicate", label: "Duplicate", command: "track.playlist_duplicate", argument: "", on: true },
                        { name: "playlistRename", label: "Rename…", command: "track.playlist_rename", argument: "", on: true },
                        { name: "playlistDeleteUnused", label: "Delete Unused", command: "track.playlist_delete_unused", argument: "", on: playlistMenu.names.length > 1 },
                        { name: "", label: "", command: "", argument: "", on: false }
                    ]
                    for (let i = 0; i < playlistMenu.names.length; ++i) {
                        const n = playlistMenu.names[i]
                        items.push({ name: "playlistChoose" + i, label: (n === active ? "\u2713  " : "     ") + n,
                                     command: "track.playlist_choose", argument: n, on: true })
                    }
                    return items
                }
                Item {
                    required property var modelData
                    width: playlistColumn.width
                    height: modelData.command.length > 0 ? 26 : 7
                    Rectangle {
                        visible: modelData.command.length === 0
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        height: 1
                        color: Theme.borderSubtle
                    }
                    SButton {
                        objectName: modelData.name
                        visible: modelData.command.length > 0
                        anchors.fill: parent
                        bold: false
                        fontPx: Theme.textBody
                        available: modelData.on
                        alignLeft: true
                        text: modelData.label
                        onClicked: {
                            playlistMenu.visible = false
                            root.shell.invokeWith(modelData.command, playlistMenu.track, modelData.argument)
                        }
                    }
                }
            }
        }
    }

    // --- Rename Playlist -----------------------------------------------------------
    // Asked for by track.playlist_rename with no name: the active playlist's
    // name, selected. Rename and Return send the command with the new name;
    // a refusal (empty, or another playlist's name) keeps the dialog open.
    MouseArea {
        anchors.fill: parent
        visible: playlistRenamePanel.visible
        z: 9
        onClicked: playlistRenamePanel.close()
    }
    Rectangle {
        id: playlistRenamePanel
        objectName: "playlistRenamePanel"
        visible: false
        z: 10
        property int track: -1
        anchors.centerIn: parent
        width: 360
        height: playlistRenameColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        function show(t, current) {
            track = t
            playlistRenameInput.text = current
            visible = true
            playlistRenameInput.forceActiveFocus()
            playlistRenameInput.selectAll()
        }
        function close() {
            visible = false
            playlistRenameInput.focus = false
            root.shell.setTextEntry(false)
        }
        function accept() {
            if (root.shell.invokeWith("track.playlist_rename", track, playlistRenameInput.text)) close()
        }

        Column {
            id: playlistRenameColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Rename Playlist"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Rectangle {
                width: parent.width
                height: 26
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: playlistRenameInput.activeFocus ? Theme.accent : Theme.borderStrong
                TextInput {
                    id: playlistRenameInput
                    objectName: "playlistRenameInput"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                    Keys.onEscapePressed: playlistRenamePanel.close()
                    Keys.onReturnPressed: playlistRenamePanel.accept()
                    Keys.onEnterPressed: playlistRenamePanel.accept()
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "playlistRenameCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: playlistRenamePanel.close()
                }
                SButton {
                    objectName: "playlistRenameOk"
                    text: "Rename"
                    implicitHeight: 24
                    onClicked: playlistRenamePanel.accept()
                }
            }
        }
    }

    // --- Rename Track, Rename Region -----------------------------------------------
    // Asked for by track.rename or region.rename with no name (a double-click
    // on a track name or on a region with the Grabber does the same): the
    // current name, selected. Rename and Return send the command with the new
    // name; a refusal (empty, or another track's name) keeps the dialog open
    // and says why inside it, until the next try or Cancel.
    MouseArea {
        anchors.fill: parent
        visible: namePanel.visible
        z: 9
        onClicked: namePanel.close()
    }
    Rectangle {
        id: namePanel
        objectName: "namePanel"
        visible: false
        z: 10
        property int track: -1   // -1: the selected region
        property string problem: ""
        anchors.centerIn: parent
        width: 360
        height: nameColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        function show(t, current) {
            track = t
            problem = ""
            nameInput.text = current
            visible = true
            nameInput.forceActiveFocus()
            nameInput.selectAll()
        }
        function close() {
            visible = false
            nameInput.focus = false
            root.shell.setTextEntry(false)
        }
        function accept() {
            if (root.shell.invokeWith(track >= 0 ? "track.rename" : "region.rename", track,
                                      nameInput.text)) close()
            else problem = root.shell.statusText
        }

        Column {
            id: nameColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                objectName: "nameTitle"
                text: namePanel.track >= 0 ? "Rename Track" : "Rename Region"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Rectangle {
                width: parent.width
                height: 26
                radius: Theme.radiusSmall
                color: Theme.window
                border.color: nameInput.activeFocus ? Theme.accent : Theme.borderStrong
                TextInput {
                    id: nameInput
                    objectName: "nameInput"
                    anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.text
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                    onTextEdited: namePanel.problem = ""
                    Keys.onEscapePressed: namePanel.close()
                    Keys.onReturnPressed: namePanel.accept()
                    Keys.onEnterPressed: namePanel.accept()
                }
            }
            Text {
                objectName: "nameProblem"
                visible: namePanel.problem.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: namePanel.problem
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "nameCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: namePanel.close()
                }
                SButton {
                    objectName: "nameOk"
                    text: "Rename"
                    implicitHeight: 24
                    onClicked: namePanel.accept()
                }
            }
        }
    }

    // --- the colour palette -----------------------------------------------------------
    // Pro Tools' Color Palette, asked for by track.colour or region.colour with
    // no colour, or a click on a track's colour bar: the palette, the current
    // colour ringed. A swatch sends the command with its number (1 to 16); a
    // region also offers its track's colour back.
    MouseArea {
        anchors.fill: parent
        visible: colourPalette.visible
        z: 9
        onClicked: colourPalette.visible = false
    }
    Rectangle {
        id: colourPalette
        objectName: "colourPalette"
        visible: false
        z: 10
        property int track: -1    // -1: the selected region
        property int current: -1  // a palette index; -1 a region wearing its track's
        property string problem: ""
        anchors.centerIn: parent
        width: paletteColumn.implicitWidth + 2 * Theme.space4
        height: paletteColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        function show(t, c) {
            track = t
            current = c
            problem = ""
            visible = true
        }
        function choose(argument) {
            if (root.shell.invokeWith(track >= 0 ? "track.colour" : "region.colour", track, argument))
                visible = false
            else problem = root.shell.statusText
        }

        Column {
            id: paletteColumn
            anchors { left: parent.left; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space3

            Text {
                objectName: "paletteTitle"
                text: colourPalette.track >= 0
                      ? "Track Colour  \u00B7  " + (root.session.trackNames[colourPalette.track] || "")
                      : "Region Colour"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Grid {
                columns: 8
                spacing: Theme.space2
                Repeater {
                    model: Theme.paletteSize
                    Rectangle {
                        id: swatch
                        required property int index
                        objectName: "colourSwatch" + index
                        width: 32; height: 32
                        radius: Theme.radius
                        color: Theme.paletteColour(index)
                        border.width: colourPalette.current === index ? 2 : (swatchArea.containsMouse ? 1 : 0)
                        border.color: Theme.text
                        Rectangle {  // the ring's inner edge, so it reads on a light swatch too
                            visible: colourPalette.current === swatch.index
                            anchors { fill: parent; margins: 2 }
                            radius: Theme.radiusSmall
                            color: "transparent"
                            border.color: Theme.window
                            border.width: 1
                        }
                        MouseArea {
                            id: swatchArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: colourPalette.choose(String(swatch.index + 1))
                        }
                    }
                }
            }
            Text {
                width: 8 * 32 + 7 * Theme.space2
                elide: Text.ElideRight
                text: colourPalette.current >= 0 ? Theme.paletteName(colourPalette.current)
                                                 : "Wears its track's colour"
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Text {
                objectName: "paletteProblem"
                visible: colourPalette.problem.length > 0
                width: 8 * 32 + 7 * Theme.space2
                wrapMode: Text.Wrap
                text: colourPalette.problem
                color: Theme.caution
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "colourFollowTrack"
                    visible: colourPalette.track < 0
                    text: "Track's Colour"
                    implicitHeight: 24
                    active: colourPalette.current < 0
                    onClicked: colourPalette.choose("track")
                }
                SButton {
                    objectName: "paletteClose"
                    text: "Close"
                    implicitHeight: 24
                    onClicked: colourPalette.visible = false
                }
            }
        }
    }

    // --- the Setup menu ----------------------------------------------------------
    MouseArea {
        anchors.fill: parent
        visible: setupMenu.visible
        z: 9
        onClicked: setupMenu.visible = false
    }
    Rectangle {
        id: setupMenu
        objectName: "setupMenu"
        visible: false
        z: 10
        function show(px, py) {
            x = Math.min(px, root.width - width - Theme.space2)
            y = py + Theme.space1
            visible = true
        }
        width: 220
        height: setupColumn.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Column {
            id: setupColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space1 }
            spacing: 2
            SButton {
                objectName: "setupLatency"
                width: setupColumn.width
                implicitHeight: 26
                bold: false
                fontPx: Theme.textBody
                text: "Latency…"
                onClicked: {
                    setupMenu.visible = false
                    root.shell.invoke("window.latency", -1)
                }
            }
            SButton {
                objectName: "setupWindowConfigs"
                width: setupColumn.width
                implicitHeight: 26
                bold: false
                fontPx: Theme.textBody
                text: "Window Configurations…"
                onClicked: {
                    setupMenu.visible = false
                    root.shell.invoke("window.configurations", -1)
                }
            }
        }
    }

    // --- Repeat: the number of copies ------------------------------------------
    // Pro Tools' Repeat dialog, asked for by edit.repeat with no count. OK
    // and Return send edit.repeat with the number through the same router;
    // a refused number leaves the dialog open with the reason on the status
    // line, so it can be corrected rather than retyped. While the field has
    // the keyboard the profile's keys are off (ShellModel::setTextEntry).
    MouseArea {
        anchors.fill: parent
        visible: repeatPanel.visible
        z: 9
        onClicked: repeatPanel.close()
    }
    Rectangle {
        id: repeatPanel
        objectName: "repeatPanel"
        property string what
        visible: false
        z: 10
        anchors.centerIn: parent
        width: 340
        height: repeatColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click inside does not close it

        function show(count, what) {
            repeatPanel.what = what
            repeatInput.text = String(count)
            visible = true
            repeatInput.forceActiveFocus()
            repeatInput.selectAll()
        }
        function close() {
            visible = false
            repeatInput.focus = false
            root.shell.setTextEntry(false)
        }
        function accept() {
            if (root.shell.invokeWith("edit.repeat", -1, repeatInput.text)) close()
        }

        Column {
            id: repeatColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Repeat"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                objectName: "repeatWhat"
                width: parent.width
                wrapMode: Text.Wrap
                text: "Copies of " + repeatPanel.what + ", end to end after the selection. What they cover is replaced."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                spacing: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Number of repeats"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Rectangle {
                    width: 72
                    height: 26
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: repeatInput.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: repeatInput
                        objectName: "repeatInput"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        horizontalAlignment: TextInput.AlignRight
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        selectedTextColor: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody; features: { "tnum": 1 } }
                        onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                        Keys.onEscapePressed: repeatPanel.close()
                        Keys.onReturnPressed: repeatPanel.accept()
                        Keys.onEnterPressed: repeatPanel.accept()
                    }
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "repeatCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: repeatPanel.close()
                }
                SButton {
                    objectName: "repeatOk"
                    text: "OK"
                    implicitHeight: 24
                    active: true
                    onClicked: repeatPanel.accept()
                }
            }
        }
    }

    // --- New Marker: its name ----------------------------------------------------
    // Pro Tools' New Memory Location dialog, asked for by marker.add while
    // stopped. OK and Return send marker.add with the name through the same
    // router; an empty field takes the offered name.
    MouseArea {
        anchors.fill: parent
        visible: markerPanel.visible
        z: 9
        onClicked: markerPanel.close()
    }
    Rectangle {
        id: markerPanel
        objectName: "markerPanel"
        property string what
        property string offered
        property bool range
        visible: false
        z: 10
        anchors.centerIn: parent
        width: 340
        height: markerColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click inside does not close it

        function show(name, what, range) {
            markerPanel.what = what
            markerPanel.range = range
            markerPanel.offered = name
            markerInput.text = name
            visible = true
            markerInput.forceActiveFocus()
            markerInput.selectAll()
        }
        function close() {
            visible = false
            markerInput.focus = false
            root.shell.setTextEntry(false)
        }
        function accept() {
            const name = markerInput.text.trim().length > 0 ? markerInput.text : markerPanel.offered
            if (root.shell.invokeWith("marker.add", -1, name)) close()
        }

        Column {
            id: markerColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "New Marker"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                objectName: "markerWhat"
                width: parent.width
                wrapMode: Text.Wrap
                text: markerPanel.range
                      ? "A selection marker " + markerPanel.what + ": recalling it selects that range again."
                      : "A marker " + markerPanel.what + "."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            Row {
                spacing: Theme.space2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Name"
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                }
                Rectangle {
                    width: 240
                    height: 26
                    radius: Theme.radiusSmall
                    color: Theme.window
                    border.color: markerInput.activeFocus ? Theme.accent : Theme.borderStrong
                    TextInput {
                        id: markerInput
                        objectName: "markerInput"
                        anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: Theme.text
                        selectionColor: Theme.selectionFill
                        selectedTextColor: Theme.text
                        font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                        onActiveFocusChanged: root.shell.setTextEntry(activeFocus)
                        Keys.onEscapePressed: markerPanel.close()
                        Keys.onReturnPressed: markerPanel.accept()
                        Keys.onEnterPressed: markerPanel.accept()
                    }
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton {
                    objectName: "markerCancel"
                    text: "Cancel"
                    implicitHeight: 24
                    onClicked: markerPanel.close()
                }
                SButton {
                    objectName: "markerOk"
                    text: "OK"
                    implicitHeight: 24
                    active: true
                    onClicked: markerPanel.accept()
                }
            }
        }
    }

    // --- the Keys menu: every profile in the key map folder -------------------
    // A file that would be refused is listed with the reason, and choosing it
    // says why on the status line, rather than being offered and then
    // quietly ignored.
    Rectangle {
        id: profilePanel
        objectName: "profilePanel"
        visible: false
        z: 10
        anchors { left: parent.left; leftMargin: Theme.space4; bottom: footer.top; bottomMargin: Theme.space2 }
        width: 440
        height: profileColumn.implicitHeight + 2 * Theme.space4
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click inside does not fall through

        Column {
            id: profileColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space4 }
            spacing: Theme.space2

            Text {
                text: "Keys"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "The key map the keyboard follows. The choice is kept for the next start."
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Repeater {
                model: root.shell.profiles
                Column {
                    required property var modelData
                    width: profileColumn.width
                    spacing: 2
                    SButton {
                        width: parent.width
                        implicitHeight: 28
                        bold: false
                        fontPx: Theme.textBody
                        available: modelData.error.length === 0
                        active: modelData.current
                        text: modelData.name + (modelData.file.length === 0 ? "  (built in)" : "")
                        onClicked: {
                            root.shell.useProfile(modelData.file)
                            if (available) profilePanel.visible = false
                        }
                    }
                    Text {
                        visible: modelData.error.length > 0
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: "refused: " + modelData.error
                        color: Theme.caution
                        font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                    }
                }
            }
            Text {
                visible: root.shell.focusKeys
                width: parent.width
                wrapMode: Text.Wrap
                text: "a-z is Commands Keyboard Focus: while it is lit, this profile's single-letter edit keys run."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            SButton {
                text: "Close"
                implicitHeight: 24
                onClicked: profilePanel.visible = false
            }
        }
    }

    // A refused gesture speaks on the status line, like a refused key; so
    // does a save or an open that worked.
    Connections {
        target: root.session
        function onRefused(why) { root.shell.say(why) }
        function onNotice(text) { root.shell.say(text) }
        function onRepeatRequested(count, what) { repeatPanel.show(count, what) }
        function onNewTracksRequested() { newTracksPanel.show() }
        function onSaveAsRequested(copy, name, where) { saveAsPanel.show(copy, name, where) }
        function onSaveAsTemplateRequested(name, where) { saveAsPanel.showTemplate(name, where) }
        function onTemplateBrowserRequested() { templatePanel.show() }
        function onPlaylistRenameRequested(track, current) { playlistRenamePanel.show(track, current) }
        function onNameRequested(track, current) { namePanel.show(track, current) }
        function onColourRequested(track, current) { colourPalette.show(track, current) }
        function onMarkerRequested(name, what, range) { markerPanel.show(name, what, range) }
    }
}
