// SPDX-License-Identifier: GPL-3.0-or-later
//
// The click and the count-off (ADR-0020), generated on the audio thread
// from the plan's ClickSegments: a beat lands on the frame the tempo map
// gives it, accented on a bar's downbeat, added to the plan's click outputs
// and never to any input, so no take can capture it.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "sonora/playback_plan.h"
#include "sonora/process_callback.h"
#include "sonora/ringbuffer.h"
#include "sonora/rt.h"
#include "sonora/transport.h"

namespace sonora {

// round(x * den / num), halves up, for num > 0 and den < 2^63: the 128-bit
// product of the map's conversion, taken one bit of x at a time so no step
// overflows 64 bits. kNoClickFrame when the result would not fit 2^62.
constexpr int64_t kNoClickFrame = INT64_MAX;
int64_t click_scale(uint64_t x, uint64_t den, uint64_t num) noexcept SONORA_RT;
// Beat j of `s`, j < s.beats; kNoClickFrame if it is past the timeline.
int64_t click_beat_frame(const ClickSegment& s, uint64_t j) noexcept SONORA_RT;

class Click {
public:
    // The count-off bars a session starts with: Pro Tools' default.
    static constexpr uint32_t kDefaultCountOffBars = 2;
    static constexpr uint32_t kMaxCountOffBars = 99;

    // Both sounds are synthesised here, sized by the rate.
    explicit Click(uint32_t sample_rate);

    // Control thread. The click sounds while the transport rolls, a take
    // included; the count-off plays before a take started with the
    // transport stopped, whether the click is on or not.
    void set_on(bool on) { on_.store(on, std::memory_order_relaxed); }
    bool on() const { return on_.load(std::memory_order_relaxed); }
    void set_count_off(bool on) { count_off_.store(on, std::memory_order_relaxed); }
    bool count_off() const { return count_off_.load(std::memory_order_relaxed); }
    // The document's transport.count_off_bars; clamped to [1, kMaxCountOffBars].
    void set_count_off_bars(uint32_t bars);
    uint32_t count_off_bars() const { return bars_.load(std::memory_order_relaxed); }
    // Session::begin_take(), before the take's transport store: the take
    // set_recording() starts as `start` (Transport::next_start()) counts
    // off first. Its start, not its number: a take in another session
    // folder can have the last one's number (ADR-0018 §1). False when the
    // ring is full.
    // `point` is where the take records from (a punch's A, a loop's A), whose
    // tempo and meter the count-off takes: the playhead is held at the
    // pre-roll start, which can be under another tempo. kAtPlayhead: the
    // playhead, for a take with no pre-roll.
    static constexpr uint64_t kAtPlayhead = ~uint64_t{0};
    bool post_count_off(uint32_t start, uint64_t point = kAtPlayhead);
    // Whether a count-off is running: begun, and not yet at its end.
    bool counting_off() const { return counting_.load(std::memory_order_relaxed); }

    // The sounds' first samples: a test finds each beat by them.
    float accent_peak() const { return accent_[0]; }
    float beat_peak() const { return beat_[0]; }

    // --- audio thread ---
    // Once per block, after the transport word is loaded and before
    // anything reads it: true while this block counts off, in which case it
    // must roll nothing and record nothing. With `usable` false (no valid
    // plan, so no tempo and no render() to end it) none begins or holds.
    // A count-off ends with the take
    // it was posted for, stopped or replaced, and with its last frame.
    bool begin_block(const Transport::Word& word, bool usable) noexcept SONORA_RT;
    // After the mix. `playhead` is the block's, locates applied; `frames`
    // the frames it rolls (to the roll's end); [loop_from, loop_to) the
    // range it wraps at, [0, 0) for none.
    void render(const PlaybackPlan& plan, const backend::AudioBlock& block, bool rolling,
                uint64_t playhead, uint32_t frames, uint64_t loop_from,
                uint64_t loop_to) noexcept SONORA_RT;

private:
    struct CountOffRecord {
        uint32_t start;
        uint32_t bars;
        uint64_t point;
    };
    // Where the timeline click is: the next beat to sound, in plan segment
    // `seg`, valid for the plan `epoch` while the block starts at `at`.
    struct Cursor {
        uint64_t epoch = 0;
        uint64_t at = ~uint64_t{0};
        uint32_t seg = 0;
        uint64_t beat = 0;
        int64_t frame = kNoClickFrame;
    };

    void seek(const PlaybackPlan& plan, uint64_t t) noexcept SONORA_RT;
    void step(const PlaybackPlan& plan) noexcept SONORA_RT;
    // The beats in timeline [t, t + n), which the block plays from `off`.
    void timeline(const PlaybackPlan& plan, const backend::AudioBlock& block, uint64_t t,
                  uint32_t off, uint32_t n) noexcept SONORA_RT;
    void start_count_off(const PlaybackPlan& plan, uint64_t playhead, uint32_t nframes) noexcept SONORA_RT;
    void count_off_beats(const backend::AudioBlock& block, uint32_t nframes) noexcept SONORA_RT;
    // The voice into the click outputs, from where it stopped to `until`.
    void sound_to(const backend::AudioBlock& block, uint32_t until) noexcept SONORA_RT;
    void trigger(const backend::AudioBlock& block, uint32_t at, bool accent) noexcept SONORA_RT;

    std::unique_ptr<float[]> accent_;
    std::unique_ptr<float[]> beat_;
    uint32_t sound_len_ = 0;

    std::atomic<bool> on_{false};
    std::atomic<bool> count_off_{false};
    std::atomic<uint32_t> bars_{kDefaultCountOffBars};
    std::atomic<bool> counting_{false};
    std::unique_ptr<RingBuffer<CountOffRecord>> posted_;

    // Audio thread only.
    const float* voice_ = nullptr;
    uint32_t voice_pos_ = 0;
    uint32_t done_ = 0;               // frames of this block the voice has sounded
    uint32_t out_[2] = {0, 1};        // this block's click outputs
    Cursor cursor_;
    // A count-off posted, not yet begun, for the take started as
    // `pending_start_`.
    bool pending_ = false;
    uint32_t pending_start_ = 0;
    uint32_t pending_bars_ = 0;
    uint64_t pending_point_ = 0;
    // The one running, for the take started as `start_`: its beats laid
    // out like a segment from frame 0, `lead_` frames into its first block
    // so that it ends on a block boundary, `total_` frames long with the
    // lead, `elapsed_` of them played. set_up_ once the length is known,
    // after the locates.
    bool counting_off_ = false;
    bool set_up_ = false;
    uint32_t start_ = 0;
    uint32_t bars_now_ = 0;
    uint64_t point_now_ = 0;
    ClickSegment seg_;
    uint64_t lead_ = 0;
    uint64_t total_ = 0;
    uint64_t elapsed_ = 0;
    uint64_t next_ = 0;
};

}  // namespace sonora
