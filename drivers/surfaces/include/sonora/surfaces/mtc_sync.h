// SPDX-License-Identifier: GPL-3.0-or-later
// MTC sync on a MidiPort (ADR-0122): chase an external master, or generate
// MTC as one. Both run on the control thread that polls the port, once per
// control tick; nothing here is called from the audio thread. One instance
// is one mode: a chase never generates and a generator never chases, so a
// session cannot end up with two masters fighting over the transport.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/surfaces/midi_port.h"
#include "sonora/surfaces/mtc.h"

namespace sonora::surfaces {

enum class MtcMode : std::uint8_t { Chase = 1, Generate = 2 };

// The session's sync settings, fixed while an instance runs. The engine's
// sample for a timecode is its timecode sample plus `offset_samples`; a
// session whose sample 0 is 01:00:00:00 has offset -3600 * sample_rate.
struct MtcSyncSettings {
    MtcMode mode = MtcMode::Chase;
    FrameRate rate = FrameRate::Fps25;
    std::uint32_t sample_rate = 48000;
    std::int64_t offset_samples = 0;
};

// --- Timeline maths (mtc.cpp) ---------------------------------------------

// The offset that puts `at_sample_zero` at engine sample 0.
std::int64_t offset_for_session_start(const Timecode& at_sample_zero, std::uint32_t sample_rate);
// Quarter frames elapsed at `samples` (floor; negative before timecode zero).
std::int64_t quarter_frame_index(std::int64_t samples, FrameRate rate, std::uint32_t sample_rate);
// The timecode a signed timecode-sample position falls in, wrapped to a day.
Timecode timecode_at(std::int64_t samples, FrameRate rate, std::uint32_t sample_rate);
// "hh:mm:ss:ff", ';' before the frames for drop-frame. For error text.
std::string format_timecode(const Timecode& tc);
// Empty when the settings can run as `expected`; otherwise why not.
std::string validate_mtc_settings(const MtcSyncSettings& s, MtcMode expected);

// --- Instances (mtc_sync.cpp) ---------------------------------------------

struct MtcSyncStats {
    std::uint32_t locates = 0;
    std::uint32_t plays = 0;
    std::uint32_t stops = 0;
    // Chase: a re-locate the take forbade (the engine was recording), and the
    // largest distance from the master seen then, in samples.
    std::uint32_t drift_events = 0;
    std::uint64_t max_drift_samples = 0;
    // Chase: a master position before the session's sample 0, and positions
    // whose rate is not the session's (still chased: the sample is right).
    std::uint32_t out_of_range = 0;
    std::uint32_t rate_mismatches = 0;
    // Generate: quarter frames and full frames written, and writes the port
    // refused (a quarter frame is dropped, a full frame is retried).
    std::uint64_t quarter_frames = 0;
    std::uint32_t full_frames = 0;
    std::uint32_t refused_writes = 0;
    // Generate: the transport moved further than one tick can catch up on
    // while rolling, so the stream restarted with a full frame.
    std::uint32_t resyncs = 0;
};

class MtcSync {
public:
    virtual ~MtcSync() = default;
    // Validates the settings against this instance's mode. False with
    // `error` set (for a held notice, not a status line): the instance then
    // does nothing at all rather than run on a wrong rate or the wrong mode.
    virtual bool start(std::string& error) = 0;
    // One control tick. `elapsed_samples` is the time since the last tick
    // counted on the audio clock by the caller, never read from a wall clock.
    virtual void tick(std::uint64_t elapsed_samples) = 0;
    virtual MtcMode mode() const = 0;
    virtual bool started() const = 0;
    virtual const MtcSyncStats& stats() const = 0;
};

// Follows a master: decodes the port, locates, rolls and stops the engine
// with transport.locate / play / stop. While the engine records it never
// locates (a jump would cut the take); it counts the drift instead.
class MtcChaseDriver final : public MtcSync {
public:
    MtcChaseDriver(const MtcSyncSettings& settings, api::EngineControl& engine, MidiPort& port);

    bool start(std::string& error) override;
    void tick(std::uint64_t elapsed_samples) override;
    MtcMode mode() const override { return MtcMode::Chase; }
    bool started() const override { return started_; }
    const MtcSyncStats& stats() const override { return stats_; }
    bool locked() const { return decoder_.locked(); }

private:
    ChaseAction decide(const Timecode& tc, const api::TransportSnapshot& t);
    void act(ChaseAction a, const api::TransportSnapshot& t);
    ChaseSnapshot to_rule(const api::TransportSnapshot& t) const;
    void send(const char* command, const std::string& argument);

    MtcSyncSettings settings_;
    api::EngineControl& engine_;
    MidiPort& port_;
    MtcDecoder decoder_;
    ChaseRule rule_;
    MtcSyncStats stats_;
    std::uint64_t silent_samples_ = 0;  // since the last position
    std::uint64_t silent_frames_ = 0;   // of those, already given to the rule
    bool started_ = false;
};

// Acts as master: follows the published transport and writes one quarter
// frame per quarter-frame period while it rolls, and a full frame on start,
// on every locate and on stop.
class MtcGenerator final : public MtcSync {
public:
    MtcGenerator(const MtcSyncSettings& settings, api::EngineControl& engine, MidiPort& port);

    bool start(std::string& error) override;
    void tick(std::uint64_t elapsed_samples) override;
    MtcMode mode() const override { return MtcMode::Generate; }
    bool started() const override { return started_; }
    const MtcSyncStats& stats() const override { return stats_; }

    // More quarter frames than this behind while rolling is a jump, not lag.
    static constexpr std::int64_t kMaxCatchUpQuarters = 8;

private:
    std::int64_t timecode_samples(std::uint64_t playhead) const;
    void full_frame_at(std::int64_t tc_samples);
    void begin_rolling(std::int64_t q_now);
    void roll(std::int64_t q_now);
    bool jumped(std::int64_t q_now) const;

    MtcSyncSettings settings_;
    api::EngineControl& engine_;
    MidiPort& port_;
    MtcEncoder encoder_;
    MtcSyncStats stats_;
    std::int64_t next_quarter_ = 0;  // quarter index the encoder sends next
    std::int64_t last_tc_samples_ = 0;
    bool rolling_ = false;
    bool have_last_ = false;
    bool full_frame_pending_ = false;  // refused by the port; retried next tick
    bool started_ = false;
};

// The instance for `settings.mode`, null for a mode that is neither (the
// caller reports it as start() would). Callers still start() it.
std::unique_ptr<MtcSync> make_mtc_sync(const MtcSyncSettings& settings, api::EngineControl& engine,
                                       MidiPort& port);

}  // namespace sonora::surfaces
