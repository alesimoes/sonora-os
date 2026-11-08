// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin chooser, opened from an empty insert slot (or "Replace plugin"
// on a full one). Categories on the left, as each plugin declares them;
// search over name and vendor at the top; the list in the middle. A plugin
// that cannot go on this track (wrong channel count, refused by the scan)
// is listed dimmed with the reason, never hidden: the operator learns why
// it is not there instead of wondering whether it was installed.
//
// Typing goes to the search field while it has focus (ShellModel's text
// entry); Escape closes, Return takes the first plugin that fits.
//
// The star marks a favourite (the interface's own, ADR-0065 §6) and
// "Favourites" lists only those; "Manage" opens the plug-in manager over
// the chooser, where plug-ins are blocked, unblocked and rescanned.
import QtQuick
import Sonora

Item {
    id: browser
    required property ShellModel shell
    property InsertModel inserts: null
    property string category: ""
    // PluginManagerModel, as main() sets it on the view's context; null
    // where nothing did (a test that loads the chooser alone).
    readonly property var manager: typeof pluginManager !== "undefined" ? pluginManager : null
    // Kept from one opening to the next: an operator who works from
    // favourites finds them again.
    property bool favouritesOnly: false
    readonly property var entries: {
        if (inserts === null || !visible) return []
        const all = (inserts.browserTrack, inserts.browse(category, search.text))
        if (!favouritesOnly || manager === null) return all
        const rev = manager.favouritesRevision
        return all.filter(e => manager.favouriteAt(e.index))
    }

    onVisibleChanged: {
        if (visible) {
            category = ""
            search.text = ""
            search.forceActiveFocus()
        } else {
            search.focus = false
            shell.setTextEntry(false)
            if (manager !== null) manager.closeManager()
        }
    }

    function chooseFirst() {
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].refusal.length === 0) {
                inserts.choose(entries[i].index)
                return
            }
        }
        shell.say(entries.length === 0 ? "no plugin matches \"" + search.text + "\""
                                       : "no plugin that matches fits this track")
    }

    // Behind the panel: dims the mixer, and a click on it closes.
    Rectangle {
        anchors.fill: parent
        color: Theme.shadow
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: browser.inserts.closeBrowser()
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.space6, 640)
        height: Math.min(parent.height - 2 * Theme.space4, 440)
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.borderStrong
        MouseArea { anchors.fill: parent }  // a click inside does not close it

        Text {
            id: title
            anchors { left: parent.left; top: parent.top; margins: Theme.space4 }
            text: "Insert plugin"
            color: Theme.text
            font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
        }
        Text {
            anchors { left: title.right; leftMargin: Theme.space3; baseline: title.baseline
                      right: manageButton.left; rightMargin: Theme.space3 }
            elide: Text.ElideRight
            text: browser.inserts !== null ? browser.inserts.browserTitle : ""
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
        }
        SButton {
            id: manageButton
            objectName: "pluginManage"
            anchors { right: closeButton.left; top: parent.top; topMargin: Theme.space3; rightMargin: Theme.space2 }
            text: "Manage"
            implicitHeight: 24
            visible: browser.manager !== null
            onClicked: {
                search.focus = false
                browser.manager.openManager()
            }
        }
        SButton {
            id: closeButton
            anchors { right: parent.right; top: parent.top; margins: Theme.space3 }
            text: "Close"
            implicitHeight: 24
            onClicked: browser.inserts.closeBrowser()
        }

        // Search.
        Rectangle {
            id: searchBox
            anchors { left: categories.right; right: parent.right; top: title.bottom
                      leftMargin: Theme.space3; rightMargin: Theme.space4; topMargin: Theme.space3 }
            height: 28
            radius: Theme.radius
            color: Theme.sunken
            border.color: search.activeFocus ? Theme.accent : Theme.borderSubtle
            TextInput {
                id: search
                objectName: "pluginSearch"
                anchors { fill: parent; leftMargin: Theme.space2; rightMargin: Theme.space2 }
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: Theme.text
                selectionColor: Theme.selectionFill
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                onActiveFocusChanged: browser.shell.setTextEntry(activeFocus)
                Keys.onEscapePressed: browser.inserts.closeBrowser()
                Keys.onReturnPressed: browser.chooseFirst()
                Keys.onEnterPressed: browser.chooseFirst()
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

        // Categories.
        Column {
            id: categories
            anchors { left: parent.left; top: searchBox.top; leftMargin: Theme.space4 }
            width: 136
            spacing: 2
            Text {
                text: "CATEGORY"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textOverline; letterSpacing: 0.6 }
            }
            SButton {
                objectName: "pluginFavourites"
                width: categories.width
                implicitHeight: 24
                bold: false
                fontPx: Theme.textSmall
                visible: browser.manager !== null
                text: "★ Favourites"
                active: browser.favouritesOnly
                onClicked: browser.favouritesOnly = !browser.favouritesOnly
            }
            Item { width: 1; height: Theme.space2; visible: browser.manager !== null }
            Repeater {
                model: browser.inserts !== null ? [""].concat(browser.inserts.categories) : []
                SButton {
                    required property string modelData
                    width: categories.width
                    implicitHeight: 24
                    bold: false
                    fontPx: Theme.textSmall
                    text: modelData.length === 0 ? "All plugins" : modelData
                    active: browser.category === modelData
                    onClicked: browser.category = modelData
                }
            }
        }

        // The list.
        ListView {
            id: list
            objectName: "pluginList"
            anchors { left: searchBox.left; right: searchBox.right; top: searchBox.bottom
                      bottom: parent.bottom; topMargin: Theme.space2; bottomMargin: Theme.space4 }
            clip: true
            spacing: 2
            model: browser.entries
            delegate: Rectangle {
                id: row
                required property var modelData
                readonly property bool fits: modelData.refusal.length === 0
                width: list.width
                height: fits ? 36 : 50
                radius: Theme.radius
                color: rowArea.containsMouse && fits ? Theme.hover : Theme.raised
                border.color: Theme.borderSubtle
                opacity: fits ? 1.0 : 0.6
                Text {
                    id: rowName
                    anchors { left: parent.left; top: parent.top; leftMargin: Theme.space2; topMargin: Theme.space1 }
                    text: row.modelData.name
                    color: Theme.text
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.DemiBold }
                }
                Text {
                    anchors { left: rowName.right; leftMargin: Theme.space2; baseline: rowName.baseline }
                    text: row.modelData.format + " · " + row.modelData.channels
                          + (row.modelData.host.length > 0 ? " · " + row.modelData.host : "")
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
                Text {
                    anchors { left: parent.left; right: parent.right; top: rowName.bottom
                              leftMargin: Theme.space2; rightMargin: Theme.space2 }
                    elide: Text.ElideRight
                    text: row.modelData.vendor + (row.modelData.category.length > 0 ? "  ·  " + row.modelData.category : "")
                    color: Theme.textSecondary
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
                Text {
                    visible: !row.fits
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                              leftMargin: Theme.space2; rightMargin: Theme.space2; bottomMargin: Theme.space1 }
                    elide: Text.ElideRight
                    text: row.modelData.refusal
                    color: Theme.caution
                    font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
                }
                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    // A plugin that does not fit still answers, with why.
                    onClicked: browser.inserts.choose(row.modelData.index)
                }
                Text {
                    visible: browser.manager !== null
                    anchors { right: parent.right; rightMargin: Theme.space2; verticalCenter: rowName.verticalCenter }
                    readonly property bool on: browser.manager !== null
                                               && (browser.manager.favouritesRevision,
                                                   browser.manager.favouriteAt(row.modelData.index))
                    text: on ? "★" : "☆"
                    color: on ? Theme.accent : Theme.textTertiary
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody }
                    MouseArea {
                        anchors { fill: parent; margins: -4 }
                        onClicked: browser.manager.toggleFavouriteAt(row.modelData.index)
                    }
                }
            }
        }
        Text {
            anchors.centerIn: list
            visible: browser.entries.length === 0
            width: list.width - 2 * Theme.space4
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: browser.inserts !== null && !browser.inserts.hasCatalog
                  ? "The engine has not sent its plugin catalog."
                  : browser.favouritesOnly && search.text.length === 0 && browser.category.length === 0
                    ? "No favourites yet: star a plugin to keep it here."
                  : search.text.length > 0 ? "No plugin matches \"" + search.text + "\"."
                  : "The engine found no plugins."
            color: Theme.textSecondary
            font { family: Theme.fontFamily; pixelSize: Theme.textBody }
        }
    }

    PluginManager {
        objectName: "pluginManager"
        anchors.fill: parent
        z: 10
        visible: browser.manager !== null && browser.manager.open
        shell: browser.shell
        plugins: browser.manager
    }
}
