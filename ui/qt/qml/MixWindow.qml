// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window: one strip per track, in the Edit window's order, and the
// 2-channel monitor bus (session-format.md, "monitor_bus") at the right.
//
// The mix is session state and is kept and shown faithfully, but the engine
// has no playback path yet, so it changes nothing anyone hears. The line
// above the strips says so: a fader that looked live and was not would be a
// control that lies.
//
// Inserts (ADR-0012 checklist 6): each strip's slots A to J, the plugin
// chooser, the parameter panel and the slot menu are drawn here, over the
// strips; a slot dragged to another strip is dropped through `dragLayer`.
// The session's delay compensation reads at the top right, in `alarm` when
// the inserts need more than its maximum.
import QtQuick
import Sonora

Rectangle {
    id: mix
    required property ShellModel shell
    required property SessionModel session
    property InsertModel inserts: null
    color: Theme.window

    Text {
        id: note
        anchors { left: parent.left; right: compensation.left; top: parent.top; margins: Theme.space3; leftMargin: Theme.space4 }
        elide: Text.ElideRight
        text: mix.inserts !== null && mix.inserts.pluginsDisabled
              ? "This engine hosts no plugins yet: inserts are kept with the session and do not run."
              : "Mix state is kept with the session. The engine has no playback path yet: faders, pan, mute and solo change nothing you hear, and the bus meters have no signal."
        color: Theme.caution
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
    }
    Text {
        id: compensation
        objectName: "compensation"
        anchors { right: parent.right; top: parent.top; margins: Theme.space3; rightMargin: Theme.space4 }
        visible: mix.inserts !== null
        width: visible ? implicitWidth : 0
        text: mix.inserts !== null ? mix.inserts.compensationText : ""
        color: mix.inserts !== null && mix.inserts.compensationShort ? Theme.alarm : Theme.textSecondary
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall; features: { "tnum": 1 }
               weight: mix.inserts !== null && mix.inserts.compensationShort ? Font.DemiBold : Font.Normal }
    }

    Item {
        id: strips
        anchors {
            left: parent.left; right: bus.left; top: note.bottom; bottom: parent.bottom
            leftMargin: Theme.space3; rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3
        }
        readonly property real stripWidth: Math.floor(width / Math.max(1, mix.session.trackCount))

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
                dragHost: dragLayer
                track: index
                name: modelData
                gainDb: mix.session.gains[index] || 0
                pan: mix.session.pans[index] || 0
                selected: mix.session.selectedTrack === index
            }
        }
    }

    ChannelStrip {
        id: bus
        anchors { right: parent.right; top: note.bottom; bottom: parent.bottom
                  rightMargin: Theme.space3; topMargin: Theme.space2; bottomMargin: Theme.space3 }
        width: 92
        shell: mix.shell
        session: mix.session
        inserts: mix.inserts
        track: -1
        name: "Monitor"
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
            fromTrack = track; fromSlot = slot; dragLayer.name = name
            targetTrack = -1; targetSlot = -1
            dragging = true
        }
        function slotUnder(x, y) {
            for (let i = 0; i < stripRepeater.count; ++i) {
                const st = stripRepeater.itemAt(i)
                if (st === null) continue
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
            slotMenu.show(track, slot, x, y)
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
