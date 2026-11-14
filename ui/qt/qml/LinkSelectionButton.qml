// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link Timeline and Edit Selection toggle for the edit toolbar (ADR-0167).
// Lit while linked, which is the default as in Pro Tools; Shift+/ flips it.
// `linked` is bound to the interface-state SelectionLink by whoever hosts it.
import QtQuick
import Sonora

SButton {
    id: b
    property bool linked: true
    signal toggled(bool linked)

    text: "Link"
    active: linked
    activeColour: Theme.accent
    onClicked: b.toggled(!b.linked)

    Shortcut {
        sequence: "Shift+/"
        onActivated: b.toggled(!b.linked)
    }
}
