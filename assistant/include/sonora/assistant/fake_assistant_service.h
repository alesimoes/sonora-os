// SPDX-License-Identifier: GPL-3.0-or-later
//
// An in-memory AssistantService for UI tests (ADR-0226). Opens nothing.
// A test scripts the outcome of an enabled, keyed request with answer().
#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "sonora/assistant/assistant_service.h"
#include "sonora/assistant/key_store.h"

namespace sonora::assistant {

class FakeAssistantService final : public AssistantService {
public:
    AssistantSettings settings() const override { return settings_; }
    bool set_enabled(bool enabled, std::string& error) override {
        if (fail_persist_) {
            error = "settings not writable";
            return false;
        }
        settings_.enabled = enabled;
        return true;
    }
    bool set_key(const std::string& key, std::string& error) override {
        return keys_.save(key, error);
    }
    void clear_key() override { keys_.clear(); }
    std::string masked_key() const override {
        std::string k;
        keys_.load(k);
        return mask_key(k);
    }

    void request_claude(const FeatureBundle&, const model::Snapshot&,
                        Completion done) override {
        ClaudeResult r;
        if (!settings_.enabled) {
            r.why = Unavailable::SwitchedOff;
        } else if (!keys_.has_key()) {
            r.why = Unavailable::NoKey;
        } else {
            ++connections_;
            r = scripted_;
        }
        done(std::move(r));
    }

    void answer(ClaudeResult r) { scripted_ = std::move(r); }
    void fail_persist(bool on) { fail_persist_ = on; }
    // Requests that got past the switch and the key, i.e. would have gone out.
    std::size_t connections() const { return connections_; }

private:
    AssistantSettings settings_;
    MemoryKeyStore keys_;
    ClaudeResult scripted_;
    bool fail_persist_ = false;
    std::size_t connections_ = 0;
};

}  // namespace sonora::assistant
