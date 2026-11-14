// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop browser (ADR-0163): a filter bar over the catalog, a list with
// audition, and Insert onto the selected track. A refusal stays at the foot
// until dismissed. Everything is LoopBrowserModel's; this draws it.
import QtQuick
import Sonora

Rectangle {
    id: panel
    property var browser: null

    width: 720
    height: 460
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.borderStrong

    MouseArea { anchors.fill: parent }

    Text {
        id: title
        anchors { left: parent.left; top: parent.top; margins: Theme.space4 }
        text: "Loops"
        color: Theme.text
        font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
    }
    Text {
        objectName: "loopSummary"
        anchors { left: title.right; leftMargin: Theme.space3; baseline: title.baseline }
        text: panel.browser !== null ? panel.browser.summary : ""
        color: Theme.textSecondary
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
    }

    // Filter bar.
    Row {
        id: bar
        anchors { left: parent.left; right: parent.right; top: title.bottom
                  margins: Theme.space4; topMargin: Theme.space3 }
        spacing: Theme.space2
        Rectangle {
            width: 200; height: 24
            radius: Theme.radius
            color: Theme.sunken
            border.color: search.activeFocus ? Theme.accent : Theme.borderSubtle
            TextInput {
                id: search
                objectName: "loopSearch"
                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                onTextChanged: if (panel.browser !== null) panel.browser.text = text
            }
            Text {
                anchors { fill: parent; leftMargin: Theme.space2 }
                verticalAlignment: Text.AlignVCenter
                visible: search.text.length === 0
                text: "Search name or tag"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
        }
        SButton {
            objectName: "loopMatchTempo"
            text: panel.browser !== null && panel.browser.sessionBpm > 0
                  ? "Match " + Math.round(panel.browser.sessionBpm) + " BPM" : "No session tempo"
            available: panel.browser !== null && panel.browser.sessionBpm > 0
            active: panel.browser !== null && panel.browser.matchTempo
            onClicked: panel.browser.matchTempo = !panel.browser.matchTempo
        }
    }

    // Category and key chips.
    Column {
        id: side
        anchors { left: parent.left; top: bar.bottom; margins: Theme.space4 }
        width: 140
        spacing: 2
        Text {
            text: "CATEGORY"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
        }
        Repeater {
            model: panel.browser !== null ? [""].concat(panel.browser.categories) : []
            SButton {
                required property string modelData
                width: side.width
                implicitHeight: 22
                bold: false
                alignLeft: true
                text: modelData.length === 0 ? "Any" : modelData
                active: panel.browser.category === modelData
                onClicked: panel.browser.category = modelData
            }
        }
        Item { width: 1; height: Theme.space3 }
        Text {
            text: "KEY"
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
        }
        Repeater {
            model: panel.browser !== null ? [""].concat(panel.browser.keys) : []
            SButton {
                required property string modelData
                width: side.width
                implicitHeight: 22
                bold: false
                alignLeft: true
                text: modelData.length === 0 ? "Any" : modelData
                active: panel.browser.key === modelData
                onClicked: panel.browser.key = modelData
            }
        }
    }

    ListView {
        id: list
        objectName: "loopList"
        anchors { left: side.right; right: parent.right; top: bar.bottom
                  bottom: footer.top; leftMargin: Theme.space3; rightMargin: Theme.space4
                  topMargin: Theme.space4; bottomMargin: Theme.space3 }
        clip: true
        spacing: 2
        model: panel.browser !== null ? panel.browser.rows : []
        delegate: Rectangle {
            id: row
            required property var modelData
            width: list.width
            height: 36
            radius: Theme.radius
            color: modelData.selected ? Theme.selectionFill : Theme.raised
            border.color: modelData.auditioning ? Theme.accent : Theme.borderSubtle
            MouseArea {
                anchors.fill: parent
                onClicked: panel.browser.select(row.modelData.id)
                onDoubleClicked: panel.browser.insertEntry(row.modelData.id)
            }
            SButton {
                id: play
                objectName: "loopAudition"
                anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                width: 28
                text: row.modelData.auditioning ? "■" : "▶"
                active: row.modelData.auditioning
                onClicked: panel.browser.audition(row.modelData.id)
            }
            Text {
                anchors { left: play.right; leftMargin: Theme.space3; right: meta.left
                          rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                elide: Text.ElideRight
                text: row.modelData.name
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
            Text {
                id: meta
                anchors { right: parent.right; rightMargin: Theme.space3; verticalCenter: parent.verticalCenter }
                text: (row.modelData.bpm > 0 ? Math.round(row.modelData.bpm) + " BPM" : "— BPM")
                      + "  ·  " + (row.modelData.key.length > 0 ? row.modelData.key : "—")
                      + "  ·  " + row.modelData.category
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
        }
        Text {
            anchors.centerIn: parent
            visible: list.count === 0
            text: "No loop matches the filters."
            color: Theme.textTertiary
            font { family: Theme.fontFamily; pixelSize: Theme.textBody }
        }
    }

    Item {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.space4 }
        height: notice.visible ? 56 : 28
        Rectangle {
            id: notice
            objectName: "loopNotice"
            visible: panel.browser !== null && panel.browser.notice.length > 0
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 24
            radius: Theme.radius
            color: Theme.sunken
            border.color: Theme.caution
            Text {
                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: 60 }
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: panel.browser !== null ? panel.browser.notice : ""
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            SButton {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 2 }
                implicitHeight: 20
                text: "Dismiss"
                onClicked: panel.browser.dismissNotice()
            }
        }
        SButton {
            objectName: "loopInsert"
            anchors { right: parent.right; bottom: parent.bottom }
            implicitHeight: 24
            text: "Insert"
            available: panel.browser !== null && panel.browser.selectedId.length > 0
            onClicked: panel.browser.insertSelected()
        }
    }
}
