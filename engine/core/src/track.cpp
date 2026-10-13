// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/track.h"

#include <cmath>

namespace sonora {

Track::Track(int id, std::string name, uint16_t channels,
             std::size_t ring_frames)
    : id_(id),
      name_(std::move(name)),
      channels_(channels),
      capture_ring_(
          std::make_unique<RingBuffer<float>>(ring_frames * channels)) {}

void Track::report_peak(float p) noexcept SONORA_RT {
    p = std::fabs(p);
    float prev = peak_.load(std::memory_order_relaxed);
    while (p > prev &&
           !peak_.compare_exchange_weak(prev, p, std::memory_order_relaxed)) {
        // prev was refreshed by the failed exchange; loop re-tests it.
    }
}

}  // namespace sonora
