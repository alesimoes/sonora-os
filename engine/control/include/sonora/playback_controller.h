// SPDX-License-Identifier: GPL-3.0-or-later
//
// The publisher of ADR-0009 §4: builds a plan from each snapshot it is
// shown, holds it pending until the disk reader reports its window
// resident, then exchanges it in and retires the old one; gates locate and
// play on residency the same way; applies the snapshot's mix to the Track
// atomics when asked. One non-real-time thread drives it, through pump():
// EngineControlServer's control thread, which is the document thread
// (ADR-0009, update "playback on the M4/M7 control server", item 2).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/plan_builder.h"

namespace sonora {

class Session;

class PlaybackController {
public:
    // Model source paths are relative to the session's active folder.
    explicit PlaybackController(Session& session);
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // Builds the snapshot's plan and makes it the pending one, cancelling
    // any plan still pending (ADR-0009 §4.2: only the newest waits). The
    // mix is not touched: a fader moved through a ParameterChange is a Track
    // atomic the document does not hold yet, and writing the document's mix
    // on every edit would snap it back (item 3 of the update above). Each
    // engine track's document_id(), and the session's name, follow the
    // snapshot, built or not, and so does the session's playback loop
    // (Session::set_loop()). False, with `error`, if the plan cannot be
    // built; what is playing does not change.
    bool show(const model::Snapshot& snapshot, std::string& error);
    // The mix alone: gain, pan, mute, solo, monitor mode, bus gain, sends
    // and aux returns. Gains and pan are clamped to what the model allows,
    // so a snapshot that never went through its validator still puts no inf
    // or NaN on the audio thread.
    void apply_mix(const model::Snapshot& snapshot);
    // The sends' and the aux returns' part of apply_mix() (ADR-0022 §6).
    // `tracks` false: the aux returns and the buses' own sends only, which
    // no change of track order moves (ADR-0023 §4).
    void apply_sends(const model::Snapshot& snapshot, bool tracks = true);
    // Engine track `track`'s gain and pan alone, from the document's track
    // there: a ride whose rider left ends without the other faders moving.
    // Nothing for a track past the document's end.
    void apply_gain(const model::Snapshot& snapshot, std::size_t track);
    // Each track's monitor mode alone, for when arming changed: an armed
    // track under --no-input-monitoring must stop sending its input at
    // once, without the faders being written.
    void apply_monitoring(const model::Snapshot& snapshot);
    // Off: an armed track sends nothing to the monitor bus, whatever its
    // mode in the document (--no-input-monitoring). Held for every snapshot
    // shown from here on, not only the next.
    void set_input_monitoring(bool on) { input_monitoring_ = on; }

    // Moves the playhead to `frame` once the window there is resident; the
    // audio thread applies it at a block start. Refused during a take; one
    // still waiting when a take starts is dropped.
    bool locate(uint64_t frame, std::string& error);
    // Rolls from the playhead once the playing plan's window there is
    // resident, with no end: a range played before does not stop it.
    void play();
    // Plays [from, to) with the pre-roll and post-roll of the snapshot last
    // shown: located to `from` less the pre-roll (cut at 0), rolled as
    // play() does, and stopped by the audio thread at `to` plus the
    // post-roll (Session::set_roll_end()). False, asking nothing, where
    // locate() refuses.
    bool play_range(uint64_t from, uint64_t to, std::string& error);
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

    // Publishes a resident pending plan, sends a resident locate and a loop
    // range whose start is resident, starts a requested roll, and frees what
    // is retired. Call often.
    void pump();
    // pump() until nothing is pending, locating, waiting to roll or holding
    // a loop range back, or the
    // timeout passes. For headless runs and tests.
    bool settle(double seconds);

    bool pending() const { return pending_ != nullptr; }
    // A plan that moves tracks between engine tracks is waiting to play
    // (ADR-0023 §4). Until it does, each engine track's atomics belong to
    // the track that was on it: nothing addressed by position (a fader
    // ride, an arm) may be written to them, and the mix asked for meanwhile
    // is written once the plan plays and no block can still be on the plan
    // before it.
    bool tracks_moving() const { return pending_mix_serial_ != 0 || adopting_ != nullptr; }
    bool locating() const { return locate_serial_ != 0 && !locate_sent_; }
    bool play_requested() const { return play_requested_; }
    bool record_requested() const { return record_requested_; }
    const PlanReport& last_report() const { return report_; }
    // The report of the plan the audio thread plays: its compensation and
    // each track's chain latency are what the state block shows.
    const PlanReport& playing_report() const { return playing_report_; }

    // How chains are bound when a plan is built (plan_builder.h). A chain's
    // latency changes only through a new plan: after the resolver's answer
    // changes, show() the snapshot again (ADR-0012 §7.3).
    void set_chain_resolver(ChainResolver resolver) { resolver_ = std::move(resolver); }
    // The session's folder `from` was renamed to `to` (ADR-0018 §3): each
    // source registered under it is the same open file under its new path,
    // so the next plan reuses its slot instead of opening it again.
    void folder_renamed(const std::string& from, const std::string& to);
    // What the next build starts from. For tests.
    const PlanContinuity& playing_continuity() const { return playing_continuity_; }
    uint64_t plans_published() const { return published_; }
    // The epoch of the plan the last successful show() built.
    uint64_t last_built_epoch() const { return last_built_epoch_; }
    // The smallest epoch among the playing plan, the pending one and every
    // retired plan not yet freed: what the audio thread may still read
    // (ADR-0009 §4.1). A slot, store or node state a plan named may be
    // reused once this is at or past the first plan built without it
    // (ADR-0012 amendment of 2026-10-14, item 2). UINT64_MAX when there is
    // no plan at all. A cancelled plan was never published: it holds none.
    uint64_t oldest_live_epoch() const;
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
    // ADR-0023 §4. Whether a mix of `snapshot`'s may go to the atomics now:
    // they hold its track order and no plan moving tracks is pending.
    bool atoms_follow(const model::Snapshot& snapshot) const;
    // `plan`'s track_mix from `snapshot`, each engine track armed if the
    // document track now on it was armed where it was before.
    void fill_track_mix(PlaybackPlan& plan, const model::Snapshot& snapshot) const;
    // Once no block can be on the plan before `plan`: its track_mix to the
    // atomics, stamped with its serial, then any mix deferred meanwhile.
    void adopt_track_mix(const PlaybackPlan& plan);

    Session& session_;
    BuildLimits limits_;
    // By the path a source resolved to, not by its id: another document can
    // give the same id another file, and the same relative path names
    // another file in another folder (ADR-0018 §1).
    std::unordered_map<std::string, uint32_t> slots_;
    ChainResolver resolver_;
    // Each build starts from the playing plan's continuity, so a plan
    // cancelled before it plays cannot move a playing edge to another line
    // (rt-safety audit of PR #48); the pending plan's becomes the playing
    // one when it is published.
    PlanContinuity playing_continuity_;
    PlanContinuity pending_continuity_;

    PlaybackPlan* pending_ = nullptr;
    uint64_t pending_gen_ = 0;
    std::vector<Dropped> retired_;     // exchanged out, freed by the epoch rule
    std::vector<Dropped> cancelled_;   // never published, freed once dropped
    PlanReport report_;                // of the plan last built
    PlanReport playing_report_;
    uint64_t published_ = 0;
    uint64_t last_built_epoch_ = 0;

    bool input_monitoring_ = true;
    // The document track whose mix each engine track's atomics hold, by
    // position; unset before the first mix is written.
    std::optional<std::vector<model::Id>> atoms_order_;
    uint32_t mix_serial_ = 0;           // the last PlaybackPlan::mix_serial given
    uint32_t pending_mix_serial_ = 0;   // pending_'s; 0 when it moves no track
    std::vector<model::Id> pending_order_;
    // Published, its mix not yet in the atomics: a block that acquired the
    // plan before it may still be running and reading them (#162 review
    // B1). Always the playing plan while set, so never freed under it.
    const PlaybackPlan* adopting_ = nullptr;
    std::vector<model::Id> adopting_order_;
    uint64_t adopting_blocks_ = 0;   // PlanExchange::blocks() at its publish
    std::optional<model::Snapshot> deferred_mix_;
    // The document's transport.pre_roll and post_roll, from the last show().
    uint64_t pre_roll_ = 0;
    uint64_t post_roll_ = 0;

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
