// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-time bounce's tap on the master bus (ui.mixer.bounce): the audio
// thread pushes what it sends the master outputs, interleaved L/R, into a
// ring a worker drains to disk. A chunk that does not fit whole is dropped
// and counted, never waited for, so the ring stays frame-aligned.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sonora/ringbuffer.h"
#include "sonora/rt.h"

namespace sonora {

class MasterTap {
public:
    // `frames` of headroom before an overrun.
    explicit MasterTap(std::size_t frames) : ring_(frames * 2 + 2) {}

    // Audio thread. Only while the transport rolls, so the file starts at
    // the first rolled frame.
    void push(const float* l, const float* r, uint32_t n) noexcept SONORA_RT {
        constexpr uint32_t kPiece = 256;
        float buf[2 * kPiece];
        for (uint32_t done = 0; done < n;) {
            const uint32_t k = n - done < kPiece ? n - done : kPiece;
            if (ring_.write_space() < std::size_t{2} * k) {
                lost_.fetch_add(k, std::memory_order_relaxed);
            } else {
                for (uint32_t i = 0; i < k; ++i) {
                    buf[2 * i] = l[done + i];
                    buf[2 * i + 1] = r[done + i];
                }
                ring_.write(buf, std::size_t{2} * k);
            }
            done += k;
        }
    }

    // Worker thread: interleaved floats, a whole number of frames.
    std::size_t read(float* dst, std::size_t frames) {
        return ring_.read(dst, frames * 2) / 2;
    }
    std::size_t frames_available() const { return ring_.read_space() / 2; }
    // Frames the audio thread had no room for: the take has a hole.
    uint64_t lost_frames() const { return lost_.load(std::memory_order_relaxed); }

private:
    RingBuffer<float> ring_;
    std::atomic<uint64_t> lost_{0};
};

}  // namespace sonora
