// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "sonora/period_guard.h"

namespace sonora::drivers {

// What describe_fatal() needs besides the guard's state. Filled on the
// control thread; gone_port is copied out from under the backend's lock.
struct FatalContext {
    uint32_t rate = 0;             // the rate the session was opened at
    uint32_t admitted_frames = 0;  // PeriodGuard::frames(): 0 if none admitted
    std::size_t max_frames = 0;    // the largest period the backend can hold
    std::string gone_port;         // the physical port whose loss was fatal
};

// AudioBackend::fatal_reason()'s text, shared by every backend so a test
// against drivers/fake asserts the sentence the appliance prints. Control
// thread only: it allocates.
inline std::string describe_fatal(PeriodGuard::State state, const FatalContext& c) {
    const std::string detail = std::to_string(state.detail);
    switch (state.why) {
        case Fatal::None:
            return {};
        case Fatal::ServerGone:
            return "the audio server went away";
        case Fatal::DeviceGone: {
            if (c.gone_port.empty()) return "a capture device went away";
            const std::string device = c.gone_port.substr(0, c.gone_port.find(':'));
            return "capture device " + device + " went away (" + c.gone_port +
                   " is gone)";
        }
        case Fatal::Stalled:
            return "the audio server ran no period for over a second";
        case Fatal::RateChanged:
            return "the audio server changed its sample rate mid-session (" +
                   std::to_string(c.rate) + " -> " + detail + " Hz)";
        case Fatal::BufferChanged:
            if (c.admitted_frames == 0) {
                return "the audio server's period of " + detail +
                       " frames is more than the " + std::to_string(c.max_frames) +
                       " this backend can hold";
            }
            return "the audio server changed its buffer size mid-session (" +
                   std::to_string(c.admitted_frames) + " -> " + detail + " frames)";
    }
    return {};
}

}  // namespace sonora::drivers
