// SPDX-License-Identifier: GPL-3.0-or-later
//
// The publisher of ADR-0009 §4: builds a plan from each snapshot it is
// shown, holds it pending until the disk reader reports its window
// resident, then exchanges it in and retires the old one; gates locate and
// play on residency the same way; applies the snapshot's mix to the Track
// atomics. One non-real-time thread drives it (the document thread, or the
// terminal loop until there is one), through pump().
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/plan_builder.h"

namespace sonora {

class Session;

class PlaybackController {
public:
    // `session_dir` is where model source paths are relative to.
    PlaybackController(Session& session, std::string session_dir);
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // Builds the snapshot's plan and makes it the pending one, cancelling
    // any plan still pending (ADR-0009 §4.2: only the newest waits). The
    // mix is applied at once: a fader is not a plan. False, with `error`,
    // if the plan cannot be built; what is playing does not change.
    bool show(const model::Snapshot& snapshot, std::string& error);
    // The mix alone: gain, pan, mute, solo, monitor mode, bus gain. Gains and
    // pan are clamped to what the model allows, so a snapshot that never went
    // through its validator still puts no inf or NaN on the audio thread.
    void apply_mix(const model::Snapshot& snapshot);
    // Off: an armed track sends nothing to the monitor bus, whatever its
    // mode in the document (--no-input-monitoring). Held for every snapshot
    // shown from here on, not only the next.
    void set_input_monitoring(bool on) { input_monitoring_ = on; }

    // Moves the playhead to `frame` once the window there is resident; the
    // audio thread applies it at a block start. Refused during a take; one
    // still waiting when a take starts is dropped.
    bool locate(uint64_t frame, std::string& error);
    // Rolls from the playhead once the playing plan's window there is
    // resident.
    void play();
    // Starts a take through `start` behind the same gate as play(): no plan
    // pending, every locate asked for applied, the window at the playhead
    // resident (ADR-0009 §4.4). `start` runs inside pump(); if it refuses,
    // take_record_error() says why.
    using StartTake = std::function<bool(std::string& why)>;
    void record(StartTake start);
    // Takes back a record() still waiting; a roll goes on.
    void cancel_record() { record_requested_ = false; }
    // Why the last take started through record() was refused, once; empty
    // if it was not.
    std::string take_record_error();
    // Stops a roll and drops a play or a record still waiting; leaves a take
    // to Session::stop().
    void stop();

    // Publishes a resident pending plan, sends a resident locate, starts a
    // requested roll, and frees what is retired. Call often.
    void pump();
    // pump() until nothing is pending, locating or waiting to roll, or the
    // timeout passes. For headless runs and tests.
    bool settle(double seconds);

    bool pending() const { return pending_ != nullptr; }
    bool locating() const { return locate_serial_ != 0 && !locate_sent_; }
    bool play_requested() const { return play_requested_; }
    bool record_requested() const { return record_requested_; }
    const PlanReport& last_report() const { return report_; }
    uint64_t plans_published() const { return published_; }
    // Locate request to the audio thread's queue, in milliseconds.
    double last_locate_ms() const { return last_locate_ms_; }
    double max_locate_ms() const { return max_locate_ms_; }

    // After AudioBackend::deactivate() has returned: frees every retired
    // plan and makes every unlinked chunk reusable (ADR-0009 §4.1).
    void after_deactivate();

private:
    struct Dropped {
        PlaybackPlan* plan;
        uint64_t generation;   // the reader stops touching it from here
    };

    uint64_t serve();
    void reclaim();
    // Whether a roll asked for at `generation` may start now.
    bool may_roll(uint64_t generation) const;
    bool registered(const model::Source& source, uint32_t& slot, std::string& error);

    Session& session_;
    const std::string dir_;
    BuildLimits limits_;
    std::unordered_map<model::Id, uint32_t> slots_;

    PlaybackPlan* pending_ = nullptr;
    uint64_t pending_gen_ = 0;
    std::vector<Dropped> retired_;     // exchanged out, freed by the epoch rule
    std::vector<Dropped> cancelled_;   // never published, freed once dropped
    PlanReport report_;
    uint64_t published_ = 0;

    bool input_monitoring_ = true;

    uint64_t locate_target_ = 0;
    // The count of applied locates at which this one is applied. Only ever
    // grows: a locate asked while the last one is sent but not yet applied
    // gets the next number, not that one's.
    uint64_t locate_serial_ = 0;
    uint64_t sent_serial_ = 0;
    uint64_t locate_gen_ = 0;
    bool locate_sent_ = true;
    std::chrono::steady_clock::time_point locate_asked_;
    double last_locate_ms_ = 0.0;
    double max_locate_ms_ = 0.0;

    bool play_requested_ = false;
    uint64_t play_gen_ = 0;

    bool record_requested_ = false;
    uint64_t record_gen_ = 0;
    StartTake start_take_;
    std::string record_error_;
};

}  // namespace sonora
