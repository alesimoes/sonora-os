// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Preferences: what the operator chooses once per machine and finds
// as it was left. Kept in a small JSON file apart from every session
// (a session carries what the session is, not how this operator works).
// Only choices the interface can honour are here. The audio device, the
// buffer and the engine's folders belong to the engine and are shown, not
// offered, until the protocol can change them.
#pragma once

#include <QString>
#include <string>

#include "edit_controller.h"
#include "time_format.h"

namespace sonora::ui {

struct Preferences {
    TimeFormat counter = TimeFormat::BarsBeats;
    GridValue grid = GridValue::Beat;
    EditMode mode = EditMode::Slip;
};

// $SONORA_PREFERENCES_FILE, else $XDG_CONFIG_HOME/sonora/preferences.json.
QString default_preferences_file();

// A missing file is the defaults. A file that cannot be read, is not what
// this version writes, or names a value it does not know is refused with the
// reason in `why` and `out` left as it was: a preference that silently reads
// as something else is worse than one that says it was not read.
bool load_preferences(const QString& file, Preferences& out, std::string& why);
bool save_preferences(const QString& file, const Preferences& prefs, std::string& why);

}  // namespace sonora::ui
