// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole window when the running engine is refused at startup for its
// versions (refusal_page.h). On the appliance an exit is a black screen that
// systemd restarts into the same refusal for ever; this page stays instead,
// so the operator reads why. It has no controls: there is nothing here the
// operator can change, and retrying cannot fix it.
import QtQuick
import Sonora

Rectangle {
    id: page
    // SocketEngine's refusal, which names both versions and what to install.
    required property string reason

    width: 1280
    height: 720
    color: Theme.window

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(760, page.width - 4 * Theme.space6)
        height: column.implicitHeight + 2 * Theme.space6
        color: Theme.panel
        radius: Theme.radius
        border { color: Theme.borderSubtle; width: 1 }

        Column {
            id: column
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space6 }
            spacing: Theme.space4

            Text {
                text: "SONORA"
                color: Theme.textTertiary
                font { family: Theme.fontFamily; pixelSize: Theme.textOverline; weight: Font.Bold; letterSpacing: 1.5 }
            }
            Text {
                objectName: "refusalTitle"
                width: column.width
                wrapMode: Text.WordWrap
                text: "This interface cannot work with the engine that is running"
                color: Theme.text
                font { family: Theme.fontFamily; pixelSize: Theme.textTitle; weight: Font.DemiBold }
            }

            // The reason, marked as the alarm it is.
            Row {
                width: column.width
                spacing: Theme.space3
                Rectangle { width: 3; height: reasonText.implicitHeight; color: Theme.alarm; radius: 1 }
                Text {
                    id: reasonText
                    objectName: "refusalReason"
                    width: column.width - 3 - Theme.space3
                    wrapMode: Text.WordWrap
                    text: page.reason
                    color: Theme.alarm
                    font { family: Theme.fontFamily; pixelSize: Theme.textBody; weight: Font.Bold }
                }
            }

            Text {
                objectName: "refusalDetail"
                width: column.width
                wrapMode: Text.WordWrap
                lineHeight: 1.2
                text: "Retrying cannot fix a version mismatch, so this interface is not trying again. "
                      + "It is not connected to the engine: nothing on this screen comes from it, and "
                      + "nothing here can change it.\n\n"
                      + "When both come from the same build, restart Sonora."
                color: Theme.textSecondary
                font { family: Theme.fontFamily; pixelSize: Theme.textBody }
            }
        }
    }
}
