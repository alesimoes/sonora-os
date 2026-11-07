// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in manager, opened from the plugin chooser's "Manage" button:
// the whole catalog, the ones that cannot be used included, and why.
// "Blocked" is the operator's own choice (Unblock undoes it); "Refused" is
// the scan's verdict, shown with its words and nothing to press. Block,
// Unblock and Rescan are engine commands: a row changes when the engine's
// next catalog says it did, never before. The star is this interface's
// favourite, which the chooser can filter by.
import QtQuick
import Sonora

Item {
    id: manager
    required property ShellModel shell
    property PluginManagerModel plugins: null

    onVisibleChanged: {
        if (visible) {
            search.text = plugins !== null ? plugins.search : ""
        } else {
            search.focus = false
            shell.setTextEntry(false)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.shadow
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: manager.plugins.closeManager()
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.space6, 820)
        height: Math.min(parent.height - 2 * Theme.space4, 560)
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }

        Text {
            id: title
            anchors { left: parent.left; top: parent.top; margins: Theme.space4 }
            text: "Plug-in manager"
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
        }
        Text {
            objectName: "managerSummary"
            anchors { left: title.right; leftMargin: Theme.space3; baseline: title.baseline
                      right: rescanButton.left; rightMargin: Theme.space3 }
            elide: Text.ElideRight
            text: manager.plugins !== null ? manager.plugins.summary : ""
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        SButton {
            id: rescanButton
            objectName: "managerRescan"
            anchors { right: closeButton.left; top: parent.top; margins: Theme.space3 }
            text: "Rescan"
            implicitHeight: 24
            available: manager.plugins !== null && manager.plugins.canRescan
            onClicked: manager.plugins.rescan()
        }
        SButton {
            id: closeButton
            anchors { right: parent.right; top: parent.top; margins: Theme.space3 }
            text: "Close"
            implicitHeight: 24
            onClicked: manager.plugins.closeManager()
        }

        Rectangle {
            id: searchBox
            anchors { left: side.right; right: parent.right; top: title.bottom
                      leftMargin: Theme.space3; rightMargin: Theme.space4; topMargin: Theme.space3 }
            height: 28
            radius: Theme.radius
            color: Theme.sunken
            border.color: search.activeFocus ? Theme.accent : Theme.borderSubtle
            TextInput {
                id: search
                objectName: "managerSearch"
                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: Theme.text
                selectionColor: Theme.selectionFill
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                onActiveFocusChanged: manager.shell.setTextEntry(activeFocus)
                onTextChanged: if (manager.plugins !== null) manager.plugins.search = text
                Keys.onEscapePressed: manager.plugins.closeManager()
            }
            Text {
                anchors { fill: parent; leftMargin: Theme.space2 }
                verticalAlignment: Text.AlignVCenter
                visible: search.text.length === 0
                text: "Search by name or vendor"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
        }

        Column {
            id: side
            anchors { left: parent.left; top: searchBox.top; leftMargin: Theme.space4 }
            width: 150
            spacing: 2
            Text {
                text: "SHOW"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
            }
            Repeater {
                model: [["all", "All plug-ins"], ["favourites", "Favourites"], ["unusable", "Blocked or refused"]]
                SButton {
                    required property var modelData
                    width: side.width
                    implicitHeight: 24
                    bold: false
                    fontPx: Theme.textSmall
                    text: modelData[1]
                    active: manager.plugins !== null && manager.plugins.show === modelData[0]
                    onClicked: manager.plugins.show = modelData[0]
                }
            }
            Item { width: 1; height: Theme.space3 }
            Text {
                text: "CATEGORY"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
            }
            Repeater {
                model: manager.plugins !== null ? [""].concat(manager.plugins.categories) : []
                SButton {
                    required property string modelData
                    width: side.width
                    implicitHeight: 24
                    bold: false
                    fontPx: Theme.textSmall
                    text: modelData.length === 0 ? "Any category" : modelData
                    active: manager.plugins !== null && manager.plugins.category === modelData
                    onClicked: manager.plugins.category = modelData
                }
            }
        }

        ListView {
            id: list
            objectName: "managerList"
            anchors { left: searchBox.left; right: searchBox.right; top: searchBox.bottom
                      bottom: noticeBar.visible ? noticeBar.top : parent.bottom
                      topMargin: Theme.space2; bottomMargin: Theme.space3 }
            clip: true
            spacing: 2
            model: manager.plugins !== null ? manager.plugins.rows : []
            delegate: Rectangle {
                id: row
                required property var modelData
                readonly property bool usable: modelData.usable
                width: list.width
                height: usable ? 40 : 56
                radius: Theme.radius
                color: Theme.raised
                border.color: modelData.blocked ? Theme.caution : Theme.borderSubtle

                Text {
                    id: star
                    anchors { left: parent.left; leftMargin: Theme.space2; verticalCenter: rowName.verticalCenter }
                    width: 18
                    text: row.modelData.favourite ? "★" : "☆"
                    color: row.modelData.favourite ? Theme.accent : Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    MouseArea {
                        anchors { fill: parent; margins: -4 }
                        onClicked: manager.plugins.toggleFavourite(row.modelData.key)
                    }
                }
                Text {
                    id: rowName
                    anchors { left: star.right; top: parent.top; leftMargin: Theme.space1; topMargin: Theme.space1 }
                    text: row.modelData.name
                    color: row.usable ? Theme.text : Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                }
                Text {
                    id: rowFormat
                    anchors { left: rowName.right; leftMargin: Theme.space2; baseline: rowName.baseline }
                    text: row.modelData.format + (row.modelData.version.length > 0 ? " " + row.modelData.version : "")
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
                // ADR-0017 Y2: a Windows build through the bridge says so.
                Rectangle {
                    visible: row.modelData.bridge.length > 0
                    anchors { left: rowFormat.right; leftMargin: Theme.space2; verticalCenter: rowFormat.verticalCenter }
                    width: bridgeText.implicitWidth + 2 * Theme.space1
                    height: bridgeText.implicitHeight + 2
                    radius: Theme.radiusSmall
                    color: Theme.sunken
                    border.color: Theme.borderStrong
                    Text {
                        id: bridgeText
                        anchors.centerIn: parent
                        text: row.modelData.bridge
                        color: Theme.textSecondary
                        font { family: Theme.fontFamily; pixelSize: Theme.textOverline }
                    }
                }
                Text {
                    anchors { left: rowName.left; right: action.left; top: rowName.bottom; rightMargin: Theme.space2 }
                    elide: Text.ElideRight
                    text: row.modelData.vendor + (row.modelData.category.length > 0 ? "  ·  " + row.modelData.category : "")
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
                Text {
                    visible: !row.usable
                    anchors { left: rowName.left; right: action.left; bottom: parent.bottom
                              rightMargin: Theme.space2; bottomMargin: Theme.space1 }
                    elide: Text.ElideRight
                    text: (row.modelData.blocked ? "Blocked: " : "Refused by its scan: ") + row.modelData.refusal
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
                Text {
                    id: stateBadge
                    visible: !row.usable
                    anchors { right: action.left; rightMargin: Theme.space2; verticalCenter: rowName.verticalCenter }
                    text: row.modelData.blocked ? "BLOCKED" : "REFUSED"
                    color: row.modelData.blocked ? Theme.caution : Theme.alarm
                    font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.DemiBold; letterSpacing: 0.6 }
                }
                SButton {
                    id: action
                    anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: parent.verticalCenter }
                    width: 84
                    implicitHeight: 24
                    visible: row.usable || row.modelData.blocked
                    text: row.modelData.blocked ? "Unblock" : "Block"
                    available: manager.plugins !== null && manager.plugins.canBlock
                    onClicked: row.modelData.blocked ? manager.plugins.unblock(row.modelData.key)
                                                     : manager.plugins.block(row.modelData.key)
                }
            }
        }
        Text {
            anchors.centerIn: list
            visible: list.count === 0
            width: list.width - 2 * Theme.space4
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: manager.plugins === null || !manager.plugins.hasCatalog
                  ? "The engine has not sent its plug-in catalog."
                  : manager.plugins.catalogSize === 0
                    ? "The engine found no plug-ins. Install LV2 or VST3 plug-ins, then Rescan."
                  : "No plug-in matches these filters."
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textBody }
        }

        // A refusal held until dismissed, never the status line's 2.5 s.
        Rectangle {
            id: noticeBar
            objectName: "managerNotice"
            visible: manager.plugins !== null && manager.plugins.notice.length > 0
            anchors { left: searchBox.left; right: searchBox.right; bottom: parent.bottom; bottomMargin: Theme.space4 }
            height: 32
            radius: Theme.radius
            color: Theme.alarmSurface
            border.color: Theme.alarm
            Text {
                anchors { left: parent.left; right: dismiss.left; verticalCenter: parent.verticalCenter
                          leftMargin: Theme.space2; rightMargin: Theme.space2 }
                elide: Text.ElideRight
                text: manager.plugins !== null ? manager.plugins.notice : ""
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
            }
            SButton {
                id: dismiss
                anchors { right: parent.right; rightMargin: Theme.space1; verticalCenter: parent.verticalCenter }
                text: "Dismiss"
                implicitHeight: 22
                onClicked: manager.plugins.dismissNotice()
            }
        }
    }
}
