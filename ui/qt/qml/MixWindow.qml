// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window: one strip per track, in the Edit window's order, and the
// 2-channel monitor bus (session-format.md, "monitor_bus") at the right.
//
// The mix is session state, and on the engine it is what the monitor bus
// plays (ADR-0009). The line above the strips says what is live and what
// the meters read (MixModel::note): on the built-in fake engine nothing is
// heard, and a fader that looked live and was not would be a control that
// lies.
//
// Inserts (ADR-0012 checklist 6): each strip's slots A to J, the plugin
// chooser, the parameter panel and the slot menu are drawn here, over the
// strips; a slot dragged to another strip is dropped through `dragLayer`.
// The session's delay compensation reads at the top right, in `alarm` when
// the inserts need more than its maximum.
//
// Sends and aux buses (ADR-0022): five send slots on every strip, A-E or
// F-J as the bank switch at the top says, as Pro Tools' Mix window views;
// the aux buses' return strips after the tracks, "+ Bus" adding one; the
// bus chooser and the send window drawn here, over the strips.
import QtQuick
import Sonora

Rectangle {
    id: mix
    required property ShellModel shell
    required property SessionModel session
    property InsertModel inserts: null
    property SendModel sends: null
    property MixModel mixer: null
    readonly property int auxCount: sends !== null ? sends.auxes.length : 0
    // Clicking the bank in use collapses the sends; the other one, or the
    // collapsed sends' switch, shows that bank.
    function pickSends(first) {
        const shown = mixer === null || mixer.showSends
        if (shown && sends.bank === first && mixer !== null) {
            mixer.showSends = false
            return
        }
        sends.setBank(first)
        if (mixer !== null) mixer.showSends = true
    }
    color: Theme.window

    Text {
        id: note
        objectName: "mixNote"
        anchors { left: parent.left; right: viewControls.left; top: parent.top; margins: Theme.space3; leftMargin: Theme.space4 }
        elide: Text.ElideRight
        text: (mix.inserts !== null && mix.inserts.pluginsDisabled
               ? "This engine hosts no plugins yet: inserts are kept with the session and do not run. "
               : "")
              + (mix.mixer !== null ? mix.mixer.note : "Mix state is kept with the session.")
        color: Theme.caution
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
    }
    // The views: which sections of a strip are drawn (MixModel, ui-design.md
    // "Mixer strip anatomy"). A collapsed section gives its height to the fader.
    Row {
        id: viewControls
        objectName: "viewControls"
        anchors { right: sendControls.left; rightMargin: Theme.space3; top: parent.top; topMargin: Theme.space1 }
        spacing: 2
        visible: mix.mixer !== null
        width: visible ? implicitWidth : 0
        SButton {
            objectName: "viewIO"
            text: "I/O"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.mixer !== null && mix.mixer.showIO
            onClicked: mix.mixer.showIO = !mix.mixer.showIO
        }
        SButton {
            objectName: "viewInsertsAE"
            text: "Ins A-E"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.mixer !== null && mix.mixer.showInsertsAE
            onClicked: mix.mixer.showInsertsAE = !mix.mixer.showInsertsAE
        }
        SButton {
            objectName: "viewInsertsFJ"
            text: "F-J"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.mixer !== null && mix.mixer.showInsertsFJ
            onClicked: mix.mixer.showInsertsFJ = !mix.mixer.showInsertsFJ
        }
    }
    Row {
        id: sendControls
        objectName: "sendControls"
        // Not anchored to `compensation`: its width would be bound to its
        // own visibility (a binding loop); the margin is its room.
        anchors { right: parent.right; top: parent.top; topMargin: Theme.space1
                  rightMargin: Theme.space4 + (compensation.visible ? compensation.implicitWidth + Theme.space3 : 0) }
        spacing: 2
        visible: mix.sends !== null
        width: visible ? implicitWidth : 0
        // The sends' own switch: A-E or F-J, and a click on the one in use
        // collapses the sends.
        SButton {
            objectName: "sendBankAE"
            text: "Sends A-E"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.sends !== null && mix.sends.bank === 0 && (mix.mixer === null || mix.mixer.showSends)
            onClicked: mix.pickSends(0)
        }
        SButton {
            objectName: "sendBankFJ"
            text: "F-J"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.sends !== null && mix.sends.bank === 5 && (mix.mixer === null || mix.mixer.showSends)
            onClicked: mix.pickSends(5)
        }
        Item { width: Theme.space2; height: 1 }
        SButton {
            objectName: "newBus"
            text: "+ Bus"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            onClicked: mix.sends.newBus()
        }
    }
    Text {
        id: compensation
        objectName: "compensation"
        anchors { right: parent.right; top: parent.top; margins: Theme.space3; rightMargin: Theme.space4 }
        visible: mix.inserts !== null
        text: mix.inserts !== null ? mix.inserts.compensationText : ""
        color: mix.inserts !== null && mix.inserts.compensationShort ? Theme.alarm : Theme.textSecondary
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 }
               weight: mix.inserts !== null && mix.inserts.compensationShort ? Font.DemiBold : Font.Normal }
    }

    // Fixed widths, never the window divided by the count: past what fits
    // the strips scroll sideways and the master stays where it is
    // (ui-design.md, "Mixer strip anatomy"). Regular until the Mix window's
    // View menu (ui.mixer.views) chooses another.
    property int stripWidth: Theme.stripRegular

    Flickable {
        id: strips
        objectName: "mixStrips"
        anchors {
            left: parent.left; right: bus.left; top: note.bottom; bottom: parent.bottom
            leftMargin: Theme.space3; rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3
        }
        clip: true
        contentWidth: (mix.session.trackCount + mix.auxCount) * mix.stripWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        readonly property real stripWidth: mix.stripWidth

        // A vertical wheel moves the strips sideways, as a mixer's does.
        WheelHandler {
            onWheel: (e) => {
                const d = e.angleDelta.x !== 0 ? e.angleDelta.x : e.angleDelta.y
                strips.contentX = Math.max(0, Math.min(strips.contentWidth - strips.width, strips.contentX - d))
            }
        }
        Rectangle {
            objectName: "stripsScrollBar"
            parent: strips
            z: 5
            visible: strips.contentWidth > strips.width
            y: strips.height - height
            height: 4
            radius: 2
            color: Theme.textTertiary
            x: strips.contentWidth > 0 ? strips.contentX / strips.contentWidth * strips.width : 0
            width: strips.contentWidth > 0 ? strips.width * strips.width / strips.contentWidth : 0
        }

        Repeater {
            id: stripRepeater
            model: mix.session.trackNames
            ChannelStrip {
                required property int index
                required property string modelData
                x: index * strips.stripWidth
                width: strips.stripWidth - 2
                height: strips.height
                shell: mix.shell
                session: mix.session
                inserts: mix.inserts
                sends: mix.sends
                mixer: mix.mixer
                dragHost: dragLayer
                track: index
                name: modelData
                gainDb: mix.session.gains[index] || 0
                pan: mix.session.pans[index] || 0
                selected: mix.session.selectedTrack === index
            }
        }
        Repeater {
            model: mix.sends !== null ? mix.sends.auxes : []
            AuxStrip {
                required property int index
                required property var modelData
                x: (mix.session.trackCount + index) * strips.stripWidth
                width: strips.stripWidth - 2
                height: strips.height
                shell: mix.shell
                session: mix.session
                sends: mix.sends
                mixer: mix.mixer
                bus: index
                info: modelData
            }
        }
    }

    ChannelStrip {
        id: bus
        anchors { right: parent.right; top: note.bottom; bottom: parent.bottom
                  rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3 }
        width: Theme.stripWide
        shell: mix.shell
        session: mix.session
        inserts: mix.inserts
        sends: mix.sends
        mixer: mix.mixer
        track: -1
        name: "Master"
        hasPan: false
        gainDb: mix.session.busGain
    }

    // --- dragging an insert between slots and strips -------------------------
    Item {
        id: dragLayer
        objectName: "insertDragLayer"
        anchors.fill: parent
        z: 20
        property bool dragging: false
        property bool copying: false
        property int fromTrack: -1
        property int fromSlot: -1
        property int targetTrack: -1
        property int targetSlot: -1
        property string name
        property real gx: 0
        property real gy: 0

        function beginDrag(track, slot, name) {
            hint.visible = false
            fromTrack = track; fromSlot = slot; dragLayer.name = name
            targetTrack = -1; targetSlot = -1
            dragging = true
        }
        function slotUnder(x, y) {
            // Strips scrolled out of view, and slots scrolled out of their
            // frame, are not drop targets: the pointer is over something else.
            const v = dragLayer.mapToItem(strips, x, y)
            if (v.x < 0 || v.y < 0 || v.x >= strips.width || v.y >= strips.height)
                return { track: -1, slot: -1 }
            for (let i = 0; i < stripRepeater.count; ++i) {
                const st = stripRepeater.itemAt(i)
                if (st === null) continue
                const f = dragLayer.mapToItem(st.insertFrame, x, y)
                if (f.x < 0 || f.y < 0 || f.x >= st.insertFrame.width || f.y >= st.insertFrame.height) continue
                const col = st.insertColumn
                const p = dragLayer.mapToItem(col, x, y)
                if (p.x < 0 || p.y < 0 || p.x >= col.width || p.y >= col.height) continue
                const c = col.childAt(p.x, p.y)
                if (c !== null && c.slot !== undefined) return { track: i, slot: c.slot }
            }
            return { track: -1, slot: -1 }
        }
        function dragTo(x, y, alt) {
            gx = x; gy = y; copying = alt
            const t = slotUnder(x, y)
            targetTrack = t.track; targetSlot = t.slot
        }
        function endDrag(alt) {
            const ft = fromTrack, fs = fromSlot, tt = targetTrack, ts = targetSlot
            dragging = false
            fromTrack = fromSlot = targetTrack = targetSlot = -1
            if (tt >= 0 && mix.inserts !== null) mix.inserts.dropInsert(ft, fs, tt, ts, alt)
        }
        function cancelDrag() {
            dragging = false
            fromTrack = fromSlot = targetTrack = targetSlot = -1
        }
        function openMenu(track, slot, x, y) {
            hint.visible = false
            slotMenu.show(track, slot, x, y)
        }
        function showHint(track, slot, text, x, y, alarm) {
            if (dragging) return
            hint.track = track; hint.slot = slot
            hint.text = text; hint.alarm = alarm
            hint.x = Math.max(Theme.space1, Math.min(x, mix.width - hint.width - Theme.space1))
            hint.y = y + 2
            hint.visible = true
        }
        function hideHint(track, slot) {
            if (hint.track === track && hint.slot === slot) hint.visible = false
        }

        Rectangle {
            id: hint
            objectName: "insertHint"
            property int track: -1
            property int slot: -1
            property string text
            property bool alarm: false
            visible: false
            width: Math.min(280, hintText.implicitWidth + 2 * Theme.space2)
            height: hintText.implicitHeight + 2 * Theme.space1
            radius: Theme.radiusSmall
            color: hint.alarm ? Theme.alarmSurface : Theme.panel
            border.color: hint.alarm ? Theme.alarm : Theme.borderStrong
            Text {
                id: hintText
                x: Theme.space2; y: Theme.space1
                width: Math.min(implicitWidth, 280 - 2 * Theme.space2)
                wrapMode: Text.Wrap
                text: hint.text
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
        }

        Rectangle {
            objectName: "insertDragGhost"
            visible: dragLayer.dragging
            x: dragLayer.gx + Theme.space2
            y: dragLayer.gy + Theme.space1
            width: ghostText.implicitWidth + 2 * Theme.space2
            height: Theme.insertSlotHeight + Theme.space1
            radius: Theme.radiusSmall
            color: Theme.raised
            border.color: Theme.accent
            opacity: 0.92
            Text {
                id: ghostText
                anchors.centerIn: parent
                text: (dragLayer.copying ? "Copy " : "Move ") + dragLayer.name
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
            }
        }
    }

    // --- the slot menu: right-click on a slot ---------------------------------
    MouseArea {
        anchors.fill: parent
        visible: slotMenu.visible
        z: 29
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: slotMenu.visible = false
    }
    Rectangle {
        id: slotMenu
        objectName: "insertMenu"
        property int track: -1
        property int slot: -1
        readonly property var info: mix.inserts !== null && track >= 0 && track < mix.inserts.strips.length
                                    ? mix.inserts.strips[track].slots[slot] : ({ present: false })
        function show(t, s, x, y) {
            track = t; slot = s
            if (!info.present) {
                mix.inserts.openBrowser(t, s)
                return
            }
            slotMenu.x = Math.min(x, mix.width - width - Theme.space2)
            slotMenu.y = Math.min(y, mix.height - height - Theme.space2)
            visible = true
        }
        visible: false
        z: 30
        width: 172
        height: menuColumn.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong

        Column {
            id: menuColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space1 }
            spacing: 2
            Repeater {
                model: [
                    { label: "Parameters", act: "panel", show: true },
                    { label: slotMenu.info.bypass ? "Bypass off" : "Bypass", act: "bypass", show: true },
                    { label: slotMenu.info.active ? "Make inactive" : "Make active", act: "active", show: true },
                    { label: "Retry the chain", act: "retry", show: slotMenu.info.faulted === true },
                    { label: "Replace plugin", act: "replace", show: true },
                    { label: "Remove", act: "remove", show: true }
                ]
                SButton {
                    required property var modelData
                    visible: modelData.show
                    width: menuColumn.width
                    implicitHeight: 22
                    bold: false
                    fontPx: Theme.textSmall
                    text: modelData.label
                    available: modelData.act !== "retry" || mix.shell.implemented("plugin.retry")
                    onClicked: {
                        const t = slotMenu.track, s = slotMenu.slot
                        slotMenu.visible = false
                        switch (modelData.act) {
                        case "panel": mix.inserts.openPanel(t, s); break
                        case "bypass": mix.inserts.toggleBypass(t, s); break
                        case "active": mix.inserts.setActive(t, s, !slotMenu.info.active); break
                        case "retry": mix.shell.invoke("plugin.retry", t); break
                        case "replace": mix.inserts.openBrowser(t, s); break
                        case "remove": mix.inserts.remove(t, s); break
                        }
                    }
                }
            }
        }
    }

    // --- the bus chooser and the send window ---------------------------------
    SendChooser {
        objectName: "sendChooser"
        anchors.fill: parent
        z: 41
        visible: mix.sends !== null && mix.sends.chooserOpen
        sends: mix.sends
    }
    SendWindow {
        objectName: "sendWindow"
        anchors { right: parent.right; top: parent.top; rightMargin: 104; topMargin: Theme.space6 }
        z: 36
        visible: mix.sends !== null && mix.sends.windowOpen
        shell: mix.shell
        session: mix.session
        sends: mix.sends
    }

    // --- the plugin chooser and the parameter panel --------------------------
    PluginBrowser {
        objectName: "pluginBrowser"
        anchors.fill: parent
        z: 40
        visible: mix.inserts !== null && mix.inserts.browserOpen
        shell: mix.shell
        inserts: mix.inserts
    }
    InsertPanel {
        objectName: "insertPanel"
        anchors.fill: parent
        z: 35
        visible: mix.inserts !== null && mix.inserts.panelOpen
        shell: mix.shell
        inserts: mix.inserts
    }
}
