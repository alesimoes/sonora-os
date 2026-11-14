// SPDX-License-Identifier: GPL-3.0-or-later
//
// FakeEngineSettings: the provider's fake of EngineSettingsStore (ADR-0007
// §7, ADR-0204), for interface and engine tests. Header-only and apart from
// FakeEngine so a test of the Preferences window needs nothing else.
//
// It holds what the engine would: validates a set whole with the contract's
// validate(), stores it whole or refuses it with the reason and keeps what it
// had, never clamps, and counts every refusal so a test can prove one was
// not swallowed. `pending` is worked out against what it was told is
// running, as the engine's would be.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "sonora/api/engine_settings.h"

namespace sonora::api {

class FakeEngineSettings final : public EngineSettingsStore {
public:
    // `running`: what the engine started with and the open session uses.
    explicit FakeEngineSettings(EngineSettings running = {})
        : running_(running), held_(std::move(running)) {}

    EngineSettingsReport set(const EngineSettingsSet& request) override {
        ++sets_;
        EngineSettingsReport r;
        r.seq = request.seq;
        r.refusal = validate(request.settings, r.reason);
        if (r.refusal == SettingsRefusal::None && fail_store_) {
            r.refusal = SettingsRefusal::NotStored;
            r.reason = "the engine's preferences file could not be written";
        }
        if (r.refusal == SettingsRefusal::None) {
            held_ = request.settings;
        } else {
            ++refusals_;
            last_reason_ = r.reason;
        }
        r.settings = held_;
        r.pending = pending();
        return r;
    }

    EngineSettingsReport report() const override {
        EngineSettingsReport r;
        r.settings = held_;
        r.pending = pending();
        return r;
    }

    // As the engine after a restart, or a session open: the held values
    // become the running ones in the part each applies to.
    void restart() { running_ = held_; }
    void open_session() { running_.automatch_ms = held_.automatch_ms; }
    // The next valid set is refused NotStored, as a full disk would make it.
    void fail_store(bool fail) { fail_store_ = fail; }

    const EngineSettings& held() const { return held_; }
    int sets() const { return sets_; }
    int refusals() const { return refusals_; }
    const std::string& last_reason() const { return last_reason_; }

private:
    uint32_t pending() const {
        uint32_t p = 0;
        if (held_.automatch_ms != running_.automatch_ms) p |= kSettingsPendingSessionOpen;
        if (held_.device_id != running_.device_id || held_.buffer_frames != running_.buffer_frames) {
            p |= kSettingsPendingRestart;
        }
        return p;
    }

    EngineSettings running_;
    EngineSettings held_;
    bool fail_store_ = false;
    int sets_ = 0;
    int refusals_ = 0;
    std::string last_reason_;
};

}  // namespace sonora::api
