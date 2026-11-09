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
    // The shell feeds every meter, so it is told where they read.
    function syncMeterPoint() {
        if (mix.mixer !== null) mix.shell.setMeterPostFader(mix.mixer.meterPoint === "post")
    }
    Component.onCompleted: syncMeterPoint()
    Connections {
        target: mix.mixer
        function onMeterPointChanged() { mix.syncMeterPoint() }
    }
    property InsertModel inserts: null
    property SendModel sends: null
    property MixModel mixer: null
    // The master's loudness (LoudnessModel); null where it is not wired.
    property LoudnessModel loudness: null
    readonly property int auxCount: sends !== null ? sends.auxes.length : 0
    // VCA masters and folders (ADR-0048): the strips' places along the
    // window, a closed folder being one card and its tracks none.
    readonly property var vcaList: mixer !== null ? mixer.vcas : []
    readonly property int slotCount: mixer !== null ? mixer.slotCount : session.trackCount
    function slotOf(i) {
        return mixer !== null && i < mixer.trackSlots.length ? mixer.trackSlots[i] : i
    }
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

    // Which strips the Mix window draws (ADR-0130): view state of this
    // window, not of the session. A hidden strip stays in its model, so it
    // plays, meters and counts in groups, VCAs and solo; only its card goes,
    // and the cards after it close up. Keys: "t3" track, "a0" aux bus,
    // "v1" VCA. The master is pinned and has no key.
    property var hiddenIds: ({})
    function isHidden(key) { return hiddenIds[key] === true }
    function setStripHidden(key, hidden) {
        if (!/^[tav][0-9]+$/.test(key) || isHidden(key) === hidden) return
        const h = Object.assign({}, hiddenIds)
        if (hidden) h[key] = true; else delete h[key]
        hiddenIds = h
    }
    function setKindHidden(kind, hidden) {
        const n = kind === "t" ? session.trackCount : kind === "a" ? auxCount : vcaCount
        for (let i = 0; i < n; ++i) setStripHidden(kind + i, hidden)
    }
    function showAll() { hiddenIds = ({}) }
    readonly property int vcaCount: vcaList.length
    // The places of the cards that are hidden and would have been drawn.
    readonly property var hiddenSlots: {
        const out = []
        for (const k of Object.keys(hiddenIds)) {
            const i = parseInt(k.substring(1))
            const slot = k[0] === "t" ? slotOf(i)
                       : k[0] === "a" ? slotCount + i : slotCount + auxCount + i
            if (slot >= 0) out.push(slot)
        }
        return out
    }
    readonly property int hiddenCount: Object.keys(hiddenIds).length
    // The column of the card at `slot`: the hidden cards before it close up.
    function column(slot) {
        let n = 0
        for (const h of hiddenSlots) if (h < slot) ++n
        return Math.max(slot, 0) - n
    }
    // Indices shift when a strip is added or removed: nothing stays hidden
    // under another strip's name.
    readonly property int trackTotal: session.trackCount
    onTrackTotalChanged: showAll()
    onAuxCountChanged: showAll()
    onVcaCountChanged: showAll()
    Connections {
        target: mix.mixer
        function onSessionOpened() { mix.showAll() }
    }

    Text {
        id: note
        objectName: "mixNote"
        anchors { left: parent.left; right: groupControls.left; top: parent.top; margins: Theme.space3; leftMargin: Theme.space4 }
        elide: Text.ElideRight
        text: (mix.inserts !== null && mix.inserts.pluginsDisabled
               ? "This engine hosts no plugins yet: inserts are kept with the session and do not run. "
               : "")
              + (mix.mixer !== null ? mix.mixer.note : "Mix state is kept with the session.")
        color: Theme.caution
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
    }
    // Groups (ADR-0041): Group makes one of the selected tracks; the others
    // act on the group of the selected track and say so when it is in none.
    Row {
        id: groupControls
        objectName: "groupControls"
        anchors { right: viewControls.left; rightMargin: Theme.space3; top: parent.top; topMargin: Theme.space1 }
        spacing: 2
        visible: mix.mixer !== null
        width: visible ? implicitWidth : 0
        readonly property int sel: mix.session.selectedTrack
        readonly property int group: mix.mixer !== null && sel >= 0 && sel < mix.mixer.trackGroups.length
                                     ? mix.mixer.trackGroups[sel] : -1
        SButton {
            objectName: "groupNew"
            text: "Group"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            onClicked: mix.shell.invoke("group.new", -1)
        }
        SButton {
            objectName: "groupSuspend"
            text: "Suspend"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: groupControls.group >= 0 && mix.mixer.groupSuspended[groupControls.group] === true
            onClicked: mix.shell.invoke("group.toggle_suspend", groupControls.sel)
        }
        SButton {
            objectName: "groupMode"
            text: groupControls.group >= 0 && mix.mixer.groupSummaries[groupControls.group].indexOf("Absolute") >= 0
                  ? "Absolute" : "Relative"
            implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            onClicked: mix.mixer.setGroupRelative(groupControls.sel, text === "Absolute")
        }
        SButton {
            objectName: "groupDissolve"
            text: "Ungroup"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            onClicked: mix.shell.invoke("group.dissolve", groupControls.sel)
        }
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
            objectName: "meterPoint"
            text: mix.mixer !== null && mix.mixer.meterPoint === "pre" ? "Pre" : "Post"
            implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.mixer !== null && mix.mixer.meterPoint === "pre"
            onClicked: mix.mixer.meterPoint = mix.mixer.meterPoint === "pre" ? "post" : "pre"
        }
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
            text: "Ins F-J"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.mixer !== null && mix.mixer.showInsertsFJ
            onClicked: mix.mixer.showInsertsFJ = !mix.mixer.showInsertsFJ
        }
        SButton {
            id: stripsButton
            objectName: "stripsButton"
            text: "Strips"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            implicitWidth: 50 + (mix.hiddenCount > 0 ? 18 : 0)
            labelOffset: mix.hiddenCount > 0 ? -9 : 0
            active: stripsPopup.visible
            onClicked: stripsPopup.toggle()
            Rectangle {
                objectName: "hiddenBadge"
                visible: mix.hiddenCount > 0
                anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                width: 14; height: 14; radius: 7
                color: Theme.caution
                Text {
                    anchors.centerIn: parent
                    text: mix.hiddenCount
                    color: Theme.window
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption - 1; weight: Font.DemiBold }
                }
            }
        }
        Item { width: Theme.space2; height: 1 }
        // Of the selected tracks (ADR-0048); a refusal is held.
        SButton {
            objectName: "newVca"
            text: "+ VCA"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            onClicked: mix.mixer.newVca("")
        }
        SButton {
            objectName: "newFolder"
            text: "Folder"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            onClicked: mix.mixer.newFolder("")
        }
        SButton {
            objectName: "viewLoudness"
            visible: mix.loudness !== null
            text: "Loudness"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
            active: mix.loudness !== null && mix.loudness.panelOpen
            onClicked: mix.loudness.togglePanel()
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
            text: "Sends F-J"; implicitHeight: 20; fontPx: Theme.textCaption; bold: false
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
            left: parent.left; right: loudnessPanel.visible ? loudnessPanel.left : bus.left; top: note.bottom; bottom: parent.bottom
            leftMargin: Theme.space3; rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3
        }
        clip: true
        contentWidth: (mix.slotCount + mix.auxCount + mix.vcaCount - mix.hiddenSlots.length) * mix.stripWidth
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
                readonly property int slot: mix.slotOf(index)
                visible: slot >= 0 && !mix.isHidden("t" + index)
                x: mix.column(slot) * strips.stripWidth
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
                visible: !mix.isHidden("a" + index)
                x: mix.column(mix.slotCount + index) * strips.stripWidth
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
        Repeater {
            model: mix.mixer !== null ? mix.mixer.folderCards : []
            FolderCard {
                required property var modelData
                x: mix.column(modelData.slot) * strips.stripWidth
                width: strips.stripWidth - 2
                height: strips.height
                mixer: mix.mixer
                info: modelData
            }
        }
        Repeater {
            model: mix.vcaList
            VcaStrip {
                required property int index
                required property var modelData
                vcaIndex: index
                faderTop: bus.faderZone.y
                visible: !mix.isHidden("v" + index)
                x: mix.column(mix.slotCount + mix.auxCount + index) * strips.stripWidth
                width: strips.stripWidth - 2
                height: strips.height
                shell: mix.shell
                session: mix.session
                mixer: mix.mixer
                info: modelData
            }
        }
    }

    // Beside the master: its loudness against a target.
    // Not made at all where the loudness is not wired: the panel reads its
    // model in every binding, shown or not.
    Loader {
        id: loudnessPanel
        anchors { right: bus.left; top: note.bottom; bottom: parent.bottom
                  rightMargin: Theme.space2; topMargin: Theme.space2; bottomMargin: Theme.space3 }
        width: 176
        active: mix.loudness !== null
        visible: active && mix.loudness.panelOpen
        sourceComponent: LoudnessPanel {
            objectName: "loudnessPanel"
            shell: mix.shell
            loudness: mix.loudness
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
                if (st === null || !st.visible) continue
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

    // --- the Strips menu: every strip listed, hidden or not (ADR-0130) ----
    MouseArea {
        anchors.fill: parent
        visible: stripsPopup.visible
        z: 27
        onClicked: stripsPopup.visible = false
    }
    Rectangle {
        id: stripsPopup
        objectName: "stripsPopup"
        visible: false
        z: 28
        width: 200
        height: Math.min(mix.height - 40, popupColumn.implicitHeight + 2 * Theme.space1)
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        function toggle() {
            const p = stripsButton.mapToItem(mix, 0, stripsButton.height)
            x = Math.max(Theme.space2, Math.min(p.x, mix.width - width - Theme.space2))
            y = p.y + 2
            visible = !visible
        }
        readonly property var rows: {
            const out = []
            const names = mix.session.trackNames
            for (let i = 0; i < names.length; ++i) out.push({ key: "t" + i, label: names[i], kind: "t" })
            const aux = mix.sends !== null ? mix.sends.auxes : []
            for (let a = 0; a < aux.length; ++a) out.push({ key: "a" + a, label: aux[a].name, kind: "a" })
            for (let v = 0; v < mix.vcaList.length; ++v) out.push({ key: "v" + v, label: mix.vcaList[v].name, kind: "v" })
            return out
        }
        Flickable {
            anchors { fill: parent; margins: Theme.space1 }
            clip: true
            contentHeight: popupColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: popupColumn
                width: parent.width
                spacing: 2
                Repeater {
                    model: [{ k: "t", label: "All audio" }, { k: "a", label: "All aux buses" }, { k: "v", label: "All VCAs" }]
                    SButton {
                        required property var modelData
                        objectName: "stripsKind_" + modelData.k
                        width: popupColumn.width; implicitHeight: 22; bold: false; fontPx: Theme.textSmall
                        alignLeft: true
                        text: modelData.label
                        readonly property bool allHidden: {
                            const mine = stripsPopup.rows.filter(r => r.kind === modelData.k)
                            return mine.length > 0 && mine.every(r => mix.isHidden(r.key))
                        }
                        active: allHidden
                        onClicked: mix.setKindHidden(modelData.k, !allHidden)
                    }
                }
                SButton {
                    objectName: "stripsShowAll"
                    width: popupColumn.width; implicitHeight: 22; bold: false; fontPx: Theme.textSmall
                    text: "Show all"
                    available: mix.hiddenCount > 0
                    onClicked: mix.showAll()
                }
                Repeater {
                    model: stripsPopup.rows
                    SButton {
                        required property var modelData
                        objectName: "stripsRow_" + modelData.key
                        width: popupColumn.width; implicitHeight: 22; bold: false; fontPx: Theme.textSmall
                        alignLeft: true
                        text: (mix.isHidden(modelData.key) ? "Hidden  " : "Shown   ") + modelData.label
                        active: mix.isHidden(modelData.key)
                        activeColour: Theme.caution
                        onClicked: mix.setStripHidden(modelData.key, !mix.isHidden(modelData.key))
                    }
                }
                Text {
                    text: "Master is always shown"
                    color: Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
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
