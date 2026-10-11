// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "sonora/ringbuffer.h"
#include "sonora/wav_writer.h"

namespace sonora {

// One recordable track. The audio callback touches only the atomics and the
// ring buffer; everything that can block lives on the disk thread.
class Track {
public:
    Track(int id, std::string name, uint16_t channels, std::size_t ring_frames);

    int id() const { return id_; }
    const std::string& name() const { return name_; }
    uint16_t channels() const { return channels_; }

    // --- state the UI and the callback share ---
    bool rec_armed() const { return rec_armed_.load(std::memory_order_relaxed); }
    void set_rec_armed(bool v) { rec_armed_.store(v, std::memory_order_relaxed); }

    bool muted() const { return muted_.load(std::memory_order_relaxed); }
    void set_muted(bool v) { muted_.store(v, std::memory_order_relaxed); }

    float gain() const { return gain_.load(std::memory_order_relaxed); }
    void set_gain(float g) { gain_.store(g, std::memory_order_relaxed); }

    // Peak since the last read, for the meters. Reading clears it.
    float take_peak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    void report_peak(float p);

    // Incremented by the callback when the ring buffer had no room, which
    // means the disk thread fell behind and samples were lost.
    uint32_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    void note_overrun() { overruns_.fetch_add(1, std::memory_order_relaxed); }

    RingBuffer<float>& capture_ring() { return *capture_ring_; }
    WavWriter& writer() { return writer_; }

private:
    const int id_;
    const std::string name_;
    const uint16_t channels_;

    std::unique_ptr<RingBuffer<float>> capture_ring_;
    WavWriter writer_;

    std::atomic<bool> rec_armed_{false};
    std::atomic<bool> muted_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<float> peak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
};

}  // namespace sonora
