// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "sonora/ringbuffer.h"
#include "sonora/rt.h"
#include "sonora/wav_writer.h"
#include "sonora/wrap_sidecar.h"

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

    // -1 is hard left, +1 hard right. Mix state, read once per block and
    // ramped like gain (ADR-0009 §4.5).
    float pan() const { return pan_.load(std::memory_order_relaxed); }
    void set_pan(float p) { pan_.store(p, std::memory_order_relaxed); }

    bool soloed() const { return soloed_.load(std::memory_order_relaxed); }
    void set_soloed(bool v) { soloed_.store(v, std::memory_order_relaxed); }

    // Another track's solo does not mute this one; its own mute still does.
    bool solo_safe() const { return solo_safe_.load(std::memory_order_relaxed); }
    void set_solo_safe(bool v) { solo_safe_.store(v, std::memory_order_relaxed); }

    // What the track sends to the monitor bus. Auto: the input while armed
    // and stopped or recording, playback otherwise (session-format.md).
    enum class Monitor : uint8_t { Auto = 0, Input = 1, Off = 2 };
    Monitor monitor() const { return monitor_.load(std::memory_order_relaxed); }
    void set_monitor(Monitor m) { monitor_.store(m, std::memory_order_relaxed); }

    // The backend input this track meters, captures and monitors: its
    // document track's, so a track moved to another engine track keeps its
    // input (ADR-0022 §4). Its own index until the control thread sets it.
    // Read once per block; never changed for an armed track during a take.
    uint32_t input() const { return input_.load(std::memory_order_relaxed); }
    void set_input(uint32_t channel) { input_.store(channel, std::memory_order_relaxed); }

    // The PlaybackPlan::mix_serial whose mix the atomics above hold
    // (ADR-0022 §4). Stored with release after them, so an audio thread that
    // reads it with acquire and finds its plan's serial reads that mix.
    uint32_t mix_serial() const { return mix_serial_.load(std::memory_order_acquire); }
    void set_mix_serial(uint32_t serial) { mix_serial_.store(serial, std::memory_order_release); }

    // Peak since the last read, for the meters. Reading clears it.
    float take_peak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    void report_peak(float p) noexcept SONORA_RT;

    // Incremented by the callback when the ring buffer had no room, which
    // means the disk thread fell behind and samples were lost.
    uint32_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    void note_overrun() { overruns_.fetch_add(1, std::memory_order_relaxed); }

    // Two files per track, so the next take's is open before the running
    // one closes (timeline-model.md, "Closing a take while rolling").
    //
    // A slot's take number is how its writer changes hands. 0: free, the
    // control thread's to open. Non-zero: the control thread has opened the
    // file for that take and published it with a release store; from then
    // the disk thread alone writes and closes it, starting at the take's
    // open boundary record, and stores 0 with release once the file is
    // closed. The control thread touches a slot's writer again only after
    // seeing 0 with acquire, or once the disk thread has been joined. The
    // audio thread never reads either.
    static constexpr int kWriterSlots = 2;
    int slot_take(int slot) const { return slot_take_[slot].load(std::memory_order_acquire); }
    // With the take, its Transport::Word::start: a take in another session
    // folder can have the number of one whose late records are still
    // coming (ADR-0018 §1), and only the start tells them apart. Stored
    // before the take, so a reader that saw the take with acquire sees it.
    void set_slot_take(int slot, int take, uint32_t start = 0) {
        slot_start_[slot].store(start, std::memory_order_relaxed);
        slot_take_[slot].store(take, std::memory_order_release);
    }
    uint32_t slot_start(int slot) const { return slot_start_[slot].load(std::memory_order_relaxed); }
    WavWriter& writer(int slot) { return writers_[slot]; }
    // A loop take's wrap list, beside its writer and handed over with it
    // (ADR-0016 §2). Not open for any other take.
    WrapSidecar& sidecar(int slot) { return sidecars_[slot]; }
    // The control thread's: the slot of the take it opened last, which
    // start_recording() reuses when it is free, so a limit set on writer()
    // between takes applies to the next one.
    WavWriter& writer() { return writers_[current_slot_]; }
    int current_slot() const { return current_slot_; }
    void set_current_slot(int slot) { current_slot_ = slot; }

    // The control thread's: the document track this engine track records
    // for, 0 if none is known. A take's file names it in its Description,
    // so recovery finds the track after a reorder, which the position alone
    // would not survive.
    uint64_t document_id() const { return document_id_; }
    void set_document_id(uint64_t id) { document_id_ = id; }

    // Written by the audio thread, inside a take's capture window only;
    // read by the disk thread; never reset (Session::process()).
    RingBuffer<float>& capture_ring() { return *capture_ring_; }

private:
    const int id_;
    const std::string name_;
    const uint16_t channels_;

    std::unique_ptr<RingBuffer<float>> capture_ring_;
    WavWriter writers_[kWriterSlots];
    WrapSidecar sidecars_[kWriterSlots];
    std::atomic<int> slot_take_[kWriterSlots] = {{0}, {0}};
    std::atomic<uint32_t> slot_start_[kWriterSlots] = {{0}, {0}};
    int current_slot_ = 0;
    uint64_t document_id_ = 0;

    std::atomic<bool> rec_armed_{false};
    std::atomic<bool> muted_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<bool> soloed_{false};
    std::atomic<bool> solo_safe_{false};
    std::atomic<Monitor> monitor_{Monitor::Auto};
    std::atomic<uint32_t> input_;
    std::atomic<uint32_t> mix_serial_{0};
    std::atomic<float> peak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
};

static_assert(std::atomic<Track::Monitor>::is_always_lock_free, "monitor_");

// The callback touches each of these every period; a lock behind any of
// them would be a lock on the audio thread.
static_assert(std::atomic<float>::is_always_lock_free, "peak_, gain_");
static_assert(std::atomic<bool>::is_always_lock_free, "rec_armed_");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "overruns_");

}  // namespace sonora
