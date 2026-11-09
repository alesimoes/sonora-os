// SPDX-License-Identifier: GPL-3.0-or-later
//
// Editing on the Tempo and Meter rulers (time.tempo_map, ADR-0140), laid over
// a ConductorRuler by EditWindow. On the Tempo ruler: drag a mark up or down
// to change its tempo (Shift: whole bpm), the value in a tooltip as it moves
// and one Undo Tempo step when the button comes up; double-click a mark to
// type its value, or empty ruler to add a change at the insertion point;
// Delete or the right-click menu removes the selected mark. On the Meter
// ruler, a double-click opens the meter picker; the meter lands on the
// nearest bar line. A refusal (the first tempo, a take open, a value out of
// range) stays beside the ruler until the next gesture or a click on it.
// The arithmetic is ui/conductor's; this file only gathers the gesture.
import QtQuick
import Sonora

Item {
    id: editor
    property var session: null
    property var shell: null
    property var ruler: null
    // Where the tooltip, the picker and the menu are drawn: they are taller
    // than the ruler and must sit over the lanes below it.
    property Item host: null
    readonly property bool tempo: ruler !== null && ruler.kind === 0
    property int selected: -1
    property string problem: ""
    // The drag in hand: the mark, where it began, how far it has gone.
    property int dragMark: -1
    property real dragFrom: 0
    property real dragDy: 0
    property bool dragWhole: false
    property bool dragMoved: false
    // The inline value field: the mark it sets (-1: a new change).
    property int fieldMark: -2
    property real pickerX: 0
    property int pickBeats: 4
    property int pickNote: 4
    // Bumped on every edit, scroll and zoom: the marks' x is read through
    // invokables, which a binding cannot watch.
    property int revision: 0
    anchors.fill: parent
    z: 2

    Connections {
        target: editor.session
        function onViewChanged() { editor.revision++ }
        function onContentChanged() { editor.revision++ }
        function onSessionChanged() { editor.revision++ }
    }
    function markX(mark) {
        return revision >= 0 && ruler ? ruler.markX(mark) : 0
    }

    function report(why) {
        problem = why
        return why === ""
    }
    function hostPoint(x, y) {
        return host ? ruler.mapToItem(host, x, y) : Qt.point(x, y)
    }

    // --- the gestures, also what the screenshot scenes drive ---
    function pressAt(x, y) {
        problem = ""
        menu.visible = false
        if (!session) return
        session.rulerClick(x)
        const m = tempo ? ruler.markAt(x) : -1
        selected = m
        if (m >= 0) {
            dragMark = m
            dragFrom = y
            dragDy = 0
            dragMoved = false
            editor.forceActiveFocus()
        }
    }
    function moveTo(y, whole) {
        if (dragMark < 0) return
        dragDy = dragFrom - y
        dragWhole = whole
        if (Math.abs(dragDy) >= 2) dragMoved = true
        tip.text = ruler.dragLabel(dragMark, dragDy, whole)
    }
    function release() {
        const m = dragMark
        dragMark = -1
        if (m >= 0 && dragMoved) report(session.conductorDragEnd(m, dragDy, dragWhole))
        dragMoved = false
    }
    function openField(mark, x) {
        problem = ""
        if (!tempo) return
        if (mark < 0) session.rulerClick(x)
        fieldMark = mark
        input.text = ruler.tempoText(mark)
        input.forceActiveFocus()
        input.selectAll()
    }
    function closeField() {
        fieldMark = -2
        input.focus = false
    }
    function submitField() {
        if (report(session.conductorTypeTempo(fieldMark, input.text))) closeField()
    }
    function removeSelected() {
        menu.visible = false
        if (selected < 0) return
        if (report(session.conductorRemove(selected))) selected = -1
    }
    function openPicker(x) {
        problem = ""
        menu.visible = false
        pickerX = x
        const parts = ruler.meterText(x).split("/")
        pickBeats = parseInt(parts[0]) || 4
        pickNote = parseInt(parts[1]) || 4
        picker.visible = true
    }
    function applyPicker() {
        if (report(session.conductorSetMeter(pickerX, pickBeats, pickNote))) picker.visible = false
    }
    function openMenu(x) {
        problem = ""
        const m = tempo ? ruler.markAt(x) : -1
        if (m >= 0) selected = m
        menu.mark = m
        menu.atX = x
        const p = hostPoint(x, ruler.height)
        menu.x = p.x
        menu.y = p.y
        menu.visible = true
    }

    onActiveFocusChanged: if (shell) shell.setTextEntry(activeFocus || input.activeFocus)
    Keys.onDeletePressed: removeSelected()
    Keys.onPressed: (e) => {
        if (e.key === Qt.Key_Backspace) { removeSelected(); e.accepted = true }
        else if (e.key === Qt.Key_Escape) { selected = -1; editor.focus = false; e.accepted = true }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: editor.tempo
        cursorShape: editor.dragMark >= 0 || (editor.tempo && containsMouse
                     && editor.ruler.markAt(mouseX) >= 0) ? Qt.SizeVerCursor : Qt.ArrowCursor
        onPressed: (m) => {
            if (m.button === Qt.RightButton) editor.openMenu(m.x)
            else editor.pressAt(m.x, m.y)
        }
        onPositionChanged: (m) => { if (pressed) editor.moveTo(m.y, (m.modifiers & Qt.ShiftModifier) !== 0) }
        onReleased: (m) => { if (m.button === Qt.LeftButton) editor.release() }
        onDoubleClicked: (m) => {
            if (m.button !== Qt.LeftButton) return
            if (editor.tempo) editor.openField(editor.ruler.markAt(m.x), m.x)
            else editor.openPicker(m.x)
        }
    }

    // The selected mark: a ring around its flag.
    Rectangle {
        visible: editor.tempo && editor.selected >= 0
        x: editor.markX(editor.selected) - 4
        y: 1
        width: 9
        height: parent.height - 2
        color: "transparent"
        border.color: Theme.accent
        border.width: 1
        radius: Theme.radiusSmall
    }

    // The inline value field, on the mark it sets.
    Rectangle {
        id: field
        visible: editor.fieldMark > -2
        x: Math.max(0, Math.min(parent.width - width, editor.markX(editor.fieldMark) - 2))
        y: 1
        width: 78
        height: parent.height - 2
        radius: Theme.radiusSmall
        color: Theme.sunken
        border.color: Theme.accent
        TextInput {
            id: input
            anchors { fill: parent; leftMargin: Theme.space1 + 2; rightMargin: 24 }
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.textOnAccent
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            onActiveFocusChanged: {
                if (editor.shell) editor.shell.setTextEntry(activeFocus)
                if (!activeFocus) editor.fieldMark = -2
            }
            Keys.onReturnPressed: editor.submitField()
            Keys.onEnterPressed: editor.submitField()
            Keys.onEscapePressed: editor.closeField()
        }
        Text {
            anchors { right: parent.right; rightMargin: Theme.space1 + 1; verticalCenter: parent.verticalCenter }
            text: "bpm"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
    }

    // The refusal, beside the ruler and held: a click dismisses it.
    Rectangle {
        objectName: "conductorRefusal"
        visible: editor.problem !== ""
        anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
        width: Math.min(parent.width * 0.6, why.implicitWidth + 2 * Theme.space2 + 14)
        height: parent.height - 4
        radius: height / 2
        color: Theme.alarmSurface
        border.color: Theme.alarm
        Text {
            id: why
            anchors { left: parent.left; leftMargin: Theme.space2; right: close.left; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
            text: editor.problem
            elide: Text.ElideRight
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
        }
        Text {
            id: close
            anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
            text: "×"
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textCaption; weight: Font.DemiBold }
        }
        MouseArea { anchors.fill: parent; onClicked: editor.problem = "" }
    }

    // The value while a mark is dragged, under the ruler at the mark.
    Rectangle {
        id: tipBox
        objectName: "conductorDragTip"
        parent: editor.host
        visible: editor.host !== null && editor.dragMark >= 0 && editor.dragMoved && tip.text !== ""
        readonly property point at: visible ? editor.hostPoint(editor.markX(editor.dragMark), editor.ruler.height) : Qt.point(0, 0)
        x: at.x - width / 2
        y: at.y + Theme.space1
        z: 60
        width: tip.implicitWidth + 2 * Theme.space2
        height: tip.implicitHeight + Theme.space1 * 2
        radius: Theme.radius
        color: Theme.raised
        border.color: Theme.accent
        Text {
            id: tip
            anchors.centerIn: parent
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall; weight: Font.DemiBold; features: { "tnum": 1 } }
        }
    }

    // A press anywhere else closes the menu or the picker, as a menu does.
    MouseArea {
        parent: editor.host
        anchors.fill: parent
        z: 59
        visible: editor.host !== null && (menu.visible || picker.visible)
        acceptedButtons: Qt.AllButtons
        onPressed: { menu.visible = false; picker.visible = false }
    }

    // Right-click: what can be done at this point of the ruler.
    Rectangle {
        id: menu
        objectName: "conductorMenu"
        parent: editor.host
        property int mark: -1
        property real atX: 0
        visible: false
        z: 60
        width: 190
        height: rows.implicitHeight + 2 * Theme.space1
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        Column {
            id: rows
            x: Theme.space1
            y: Theme.space1
            width: parent.width - 2 * Theme.space1
            SButton {
                width: parent.width; alignLeft: true; bold: false
                visible: editor.tempo
                text: menu.mark >= 0 ? "Edit Tempo Value…" : "Add Tempo Change Here…"
                onClicked: { menu.visible = false; editor.openField(menu.mark, menu.atX) }
            }
            SButton {
                width: parent.width; alignLeft: true; bold: false
                visible: editor.tempo && menu.mark >= 0
                text: "Remove Tempo Change"
                onClicked: editor.removeSelected()
            }
            SButton {
                width: parent.width; alignLeft: true; bold: false
                visible: !editor.tempo
                text: "Change Meter…"
                onClicked: { menu.visible = false; editor.openPicker(menu.atX) }
            }
        }
    }

    // The meter picker: beats over note value, the result read large.
    Rectangle {
        id: picker
        objectName: "conductorMeterPicker"
        parent: editor.host
        visible: false
        readonly property point at: visible ? editor.hostPoint(editor.pickerX, editor.ruler.height) : Qt.point(0, 0)
        x: Math.max(0, Math.min((editor.host ? editor.host.width : 0) - width, at.x - width / 2))
        y: at.y + Theme.space1
        z: 60
        width: 8 * 30 + 2 * Theme.space3
        height: body.implicitHeight + 2 * Theme.space3
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // clicks in the gaps stay here
        Column {
            id: body
            x: Theme.space3
            y: Theme.space3
            width: parent.width - 2 * Theme.space3
            spacing: Theme.space2
            Row {
                width: parent.width
                Text {
                    width: parent.width - big.width
                    text: "METER"
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 1 }
                }
                Text {
                    id: big
                    text: editor.pickBeats + "/" + editor.pickNote
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold; features: { "tnum": 1 } }
                }
            }
            Text {
                text: "Beats in the bar"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Grid {
                columns: 8
                spacing: 2
                Repeater {
                    model: editor.ruler ? editor.ruler.meterBeats() : []
                    SButton {
                        required property int modelData
                        width: 28; implicitHeight: 22
                        fontPx: Theme.textCaption
                        text: modelData
                        active: editor.pickBeats === modelData
                        onClicked: editor.pickBeats = modelData
                    }
                }
            }
            Text {
                text: "Note value"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Row {
                spacing: 2
                Repeater {
                    model: editor.ruler ? editor.ruler.meterNotes() : []
                    SButton {
                        required property int modelData
                        width: 46; implicitHeight: 22
                        fontPx: Theme.textCaption
                        text: "/" + modelData
                        active: editor.pickNote === modelData
                        onClicked: editor.pickNote = modelData
                    }
                }
            }
            Text {
                width: parent.width
                visible: editor.problem !== ""
                text: editor.problem
                wrapMode: Text.WordWrap
                color: Theme.alarm
                font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.space2
                SButton { text: "Cancel"; onClicked: { picker.visible = false; editor.problem = "" } }
                SButton { text: "Set Meter"; active: true; onClicked: editor.applyPicker() }
            }
        }
    }
}
