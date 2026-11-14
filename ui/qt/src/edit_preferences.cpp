// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Preferences: the choices the operator keeps per machine (see
// preferences.h), applied to the edit state as they are made and at start.
#include "session_model.h"
#include "sonora/api/state_block.h"
#include "transport_prefs.h"

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
    applyTransportPrefs();
    sync();
}

void SessionModel::applyTransportPrefs() {
    // No engine: the preference waits for the first state block after connect.
    if (!engine_alive_ || !send_command_) return;
    transport_prefs_pending_ = false;
    const uint32_t flags = (engine_click_ ? api::kTransportClick : 0u) |
                           (engine_count_off_ ? api::kTransportCountOff : 0u);
    const TransportToggles plan = plan_transport_toggles(prefs_, flags);
    if (plan.toggle_click) send_command_(QStringLiteral("transport.toggle_click"), QString());
    if (plan.toggle_count_off) {
        send_command_(QStringLiteral("transport.toggle_count_off"), QString());
    }
    // The block still shows the old flags until the engine answers; remember
    // what was asked so a second choice plans against it, not against stale.
    engine_click_ = prefs_.click_at_start;
    engine_count_off_ = prefs_.count_off_at_start;
}

void SessionModel::noteEngineToggles(const TransportSnapshot& t, bool alive) {
    if (!alive) {
        engine_alive_ = false;
        transport_prefs_pending_ = true;   // the next engine to answer gets the plan
        return;
    }
    engine_alive_ = true;
    engine_click_ = t.click;
    engine_count_off_ = t.count_off;
    if (transport_prefs_pending_) applyTransportPrefs();
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
    } else if (what == QLatin1String("click") || what == QLatin1String("countoff")) {
        bool& field = what == QLatin1String("click") ? next.click_at_start
                                                     : next.count_off_at_start;
        if (value == QLatin1String("on") || value == QLatin1String("off")) {
            field = value == QLatin1String("on");
            known = true;
        }
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
