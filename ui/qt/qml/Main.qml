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
        onEditMenuRequested: (x, y) => editMenu.show(x, y)
        editMenuOpen: editMenu.visible
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
        }
        MixWindow {
            objectName: "mixWindow"
            anchors.fill: parent
            visible: root.session.window === "mix"
            shell: root.shell
            session: root.session
            inserts: root.inserts
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

    // --- the Transport window, floating over either window ---
    Loader {
        id: transportLoader
        objectName: "transportLoader"
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
        property bool discard: false
        function show() {
            entries = root.session.sessionList()
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
            SButton {
                text: "Cancel"
                implicitHeight: 24
                onClicked: openPanel.visible = false
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
                    { name: "", label: "", command: "", on: false },
                    { name: "editCut", label: "Cut", command: "edit.cut", on: true },
                    { name: "editCopy", label: "Copy", command: "edit.copy", on: true },
                    { name: "editPaste", label: "Paste", command: "edit.paste", on: root.session.canPaste },
                    { name: "editClear", label: "Clear", command: "edit.clear", on: true },
                    { name: "editDuplicate", label: "Duplicate", command: "edit.duplicate", on: true }
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
    }
}
