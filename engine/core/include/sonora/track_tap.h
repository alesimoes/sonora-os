// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place's tap on one track (ADR-0051): the audio thread pushes what
// the track's plug-in chain sent out, before the fader and the pan, as
// interleaved L/R into a ring a worker drains to disk. The chain's output is
// its latency late, so the tap says that latency with every block and the
// worker drops that many frames from the head of the file. A chain that did
// not play a block (not bound, silent, run in pieces) is counted, never
// pushed as dry audio, and the render is then refused: a file with a hole,
// or with an unprocessed stretch, is never produced (ADR-0012 §3).
// A chunk that does not fit whole is dropped and counted, never waited for.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sonora/ringbuffer.h"
#include "sonora/rt.h"

namespace sonora {

class TrackTap {
public:
    // `engine_track` is the index into Session::tracks(); `frames` of
    // headroom before an overrun.
    TrackTap(uint32_t engine_track, std::size_t frames)
        : engine_track_(engine_track), ring_(frames * 2 + 2) {}

    uint32_t engine_track() const { return engine_track_; }

    // Audio thread. Only while the transport rolls.
    void push(const float* l, const float* r, uint32_t n, uint32_t latency) noexcept SONORA_RT {
        latency_.store(latency, std::memory_order_relaxed);
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

    // Audio thread: `n` frames the chain did not play.
    void unplayed(uint32_t n) noexcept SONORA_RT {
        unplayed_.fetch_add(n, std::memory_order_relaxed);
    }

    // Worker thread: interleaved floats, a whole number of frames.
    std::size_t read(float* dst, std::size_t frames) { return ring_.read(dst, frames * 2) / 2; }
    std::size_t frames_available() const { return ring_.read_space() / 2; }
    uint64_t lost_frames() const { return lost_.load(std::memory_order_relaxed); }
    uint64_t unplayed_frames() const { return unplayed_.load(std::memory_order_relaxed); }
    // The chain's latency in the last block pushed; 0 before any.
    uint32_t latency() const { return latency_.load(std::memory_order_relaxed); }

private:
    const uint32_t engine_track_;
    RingBuffer<float> ring_;
    std::atomic<uint64_t> lost_{0};
    std::atomic<uint64_t> unplayed_{0};
    std::atomic<uint32_t> latency_{0};
};

}  // namespace sonora
