// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's side of "bounded rate" for PluginTelemetry (ADR-0057): at
// most kTelemetryMaxHz messages per insert, and none for a frame the client
// already has. Control thread only; it allocates, so never the audio thread.
#pragma once

#include <cstdint>
#include <map>

#include "sonora/api/messages.h"

namespace sonora::api {

class TelemetryThrottle {
public:
    // True when a frame of `sequence` read at `now_ns` for `insert` should be
    // sent; it then counts as sent. A sequence that went backwards is a plugin
    // that restarted and is sent at once.
    bool admit(uint64_t insert, uint32_t sequence, uint64_t now_ns) {
        auto it = last_.find(insert);
        if (it == last_.end()) {
            last_[insert] = Sent{sequence, now_ns};
            return true;
        }
        Sent& s = it->second;
        if (sequence == s.sequence) return false;
        if (sequence > s.sequence && now_ns - s.at_ns < kMinIntervalNs) return false;
        s = Sent{sequence, now_ns};
        return true;
    }
    // The insert is gone; the next frame of a new one of the same id is new.
    void forget(uint64_t insert) { last_.erase(insert); }

    static constexpr uint64_t kMinIntervalNs = 1000000000ull / kTelemetryMaxHz;

private:
    struct Sent {
        uint32_t sequence;
        uint64_t at_ns;
    };
    std::map<uint64_t, Sent> last_;
};

}  // namespace sonora::api
