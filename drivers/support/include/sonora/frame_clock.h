// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::drivers {

// Widens a server's 32-bit frame counter to AudioBlock::frame_time's 64 bits.
// JACK's wraps after about a day at 48 kHz. Audio thread only: it is called
// once per period and holds no state anyone else reads.
class FrameClock {
public:
    uint64_t widen(uint32_t now) noexcept SONORA_RT {
        if (!started_) {
            started_ = true;
            wide_ = now;
        } else {
            // Unsigned subtraction is the forward distance even across a
            // wrap, so the result never decreases.
            wide_ += static_cast<uint32_t>(now - last_);
        }
        last_ = now;
        return wide_;
    }

private:
    bool started_ = false;
    uint32_t last_ = 0;
    uint64_t wide_ = 0;
};

}  // namespace sonora::drivers
