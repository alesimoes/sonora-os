// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Preferences: the choices the operator keeps per machine (see
// preferences.h), applied to the edit state as they are made and at start.
#include "session_model.h"

namespace sonora::ui {

void SessionModel::setPreferencesOpen(bool on) {
    if (on == prefs_open_) return;
    prefs_open_ = on;
    emit preferencesChanged();
}

void SessionModel::applyPreferences() {
    setCounterFormat(prefs_.counter);
    ctl_.set_grid(prefs_.grid);
    ctl_.set_mode(prefs_.mode);
    sync();
}

void SessionModel::setPreferencesFile(const QString& file) {
    prefs_file_ = file;
    prefs_refusal_.clear();
    Preferences loaded = prefs_;
    std::string why;
    if (load_preferences(file, loaded, why)) {
        prefs_ = loaded;
        applyPreferences();
    } else {
        // The defaults stay, and the window says why the file was not used.
        prefs_refusal_ = QString::fromStdString(why);
    }
    emit preferencesChanged();
}

bool SessionModel::setPreference(const QString& what, const QString& value) {
    prefs_refusal_.clear();
    Preferences next = prefs_;
    bool known = false;
    const auto choose = [&](auto& field, auto first, auto last,
                            const char* (*name)(decltype(first))) {
        for (int i = static_cast<int>(first); i <= static_cast<int>(last); ++i) {
            const auto e = static_cast<decltype(first)>(i);
            if (value == QString::fromLatin1(name(e))) {
                field = e;
                known = true;
            }
        }
    };
    if (what == QLatin1String("counter")) {
        choose(next.counter, TimeFormat::Timecode, TimeFormat::Samples, time_format_name);
    } else if (what == QLatin1String("grid")) {
        choose(next.grid, GridValue::Bar, GridValue::Sixteenth, grid_value_name);
    } else if (what == QLatin1String("mode")) {
        choose(next.mode, EditMode::Shuffle, EditMode::Grid, edit_mode_name);
    }
    if (!known) {
        prefs_refusal_ = QStringLiteral("no preference \"%1\" can be \"%2\"").arg(what, value);
        emit preferencesChanged();
        return false;
    }
    prefs_ = next;
    applyPreferences();
    std::string why;
    if (!prefs_file_.isEmpty() && !save_preferences(prefs_file_, prefs_, why)) {
        prefs_refusal_ = QStringLiteral("in use now, but not kept for the next start: ") +
                         QString::fromStdString(why);
    }
    emit preferencesChanged();
    return prefs_refusal_.isEmpty();
}

}  // namespace sonora::ui
