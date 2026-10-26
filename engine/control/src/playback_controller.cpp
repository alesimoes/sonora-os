// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/playback_controller.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include "sonora/session.h"

namespace sonora {
namespace {

// The document validates count_off_bars against the model's bound and the
// click clamps to its own; they are the same number or one rejects what the
// other plays.
static_assert(Click::kMaxCountOffBars == model::kMaxCountOffBars,
              "the count-off bar limit differs between the model and the click");

Track::Monitor monitor_of(model::MonitorMode m) {
    switch (m) {
        case model::MonitorMode::Input:
            return Track::Monitor::Input;
        case model::MonitorMode::Off:
            return Track::Monitor::Off;
        case model::MonitorMode::Auto:
            break;
    }
    return Track::Monitor::Auto;
}

float pan_of(float pan) {
    if (std::isnan(pan)) return 0.0f;
    return std::clamp(pan, -1.0f, 1.0f);
}

}  // namespace

PlaybackController::PlaybackController(Session& session) : session_(session) {
    const PlaybackConfig& pb = session.config().playback;
    limits_.engine_tracks = static_cast<uint32_t>(session.tracks().size());
    limits_.channels_per_track = session.config().channels_per_track;
    limits_.max_segments = pb.max_segments;
    limits_.max_chunk_slots = pb.max_chunk_slots;
    limits_.chunk_frames = pb.chunk_frames;
    limits_.window_frames = session.window_frames();
    limits_.pool_chunks = session.reader().pool_chunks();
    limits_.max_delay_frames = session.max_delay_frames();
    limits_.delay_lines = session.delay_lines();
    playing_continuity_ = PlanContinuity(session.delay_lines());
}

PlaybackController::~PlaybackController() {
    // The plans this side still owns may be in the reader's hands: drop
    // them from its set, and free them only once it says so. Never on a
    // timeout; a reader that does not answer leaks them instead.
    DiskReader& reader = session_.reader();
    PlaybackPlan* const pending = pending_;
    pending_ = nullptr;
    const uint64_t gen = serve();
    if (reader.running()) {
        for (int i = 0; i < 2000 && reader.adopted() < gen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        reader.run_pass();
    }
    if (reader.adopted() < gen) return;
    if (pending) destroy_plan(pending);
    for (const Dropped& d : cancelled_) destroy_plan(d.plan);
}

bool PlaybackController::registered(const model::Source& source, uint32_t& slot,
                                    std::string& error) {
    std::string path = *session_.folder() + "/" + source.path;
    const auto it = slots_.find(path);
    if (it != slots_.end()) {
        slot = it->second;
        return true;
    }
    // The engine's table is append-only, so a slot, once given, is the
    // file's for the run. A file is not rewritten under its name: a take
    // number is never reused in a folder (Session::next_free_take()).
    if (!session_.reader().add_source(path, slot, error)) return false;
    slots_.emplace(std::move(path), slot);
    return true;
}

void PlaybackController::folder_renamed(const std::string& from, const std::string& to) {
    const std::string prefix = from + "/";
    std::unordered_map<std::string, uint32_t> moved;
    moved.reserve(slots_.size());
    for (auto& [path, slot] : slots_) {
        if (path.compare(0, prefix.size(), prefix) == 0) {
            moved.emplace(to + "/" + path.substr(prefix.size()), slot);
        } else {
            moved.emplace(path, slot);
        }
    }
    slots_ = std::move(moved);
}

uint64_t PlaybackController::serve() {
    return session_.reader().serve(session_.plans().current(), pending_,
                                   static_cast<int64_t>(locate_target_), locate_serial_);
}

void PlaybackController::apply_mix(const model::Snapshot& snapshot) {
    const auto& tracks = snapshot.edit->tracks;
    for (std::size_t i = 0; i < tracks.size() && i < session_.tracks().size(); ++i) {
        const model::TrackMix* m = snapshot.mix.find(tracks[i]->id);
        if (!m) continue;
        Track& t = *session_.tracks()[i];
        t.set_gain(gain_of_db(m->gain_db));
        t.set_pan(pan_of(m->pan));
        t.set_muted(m->mute);
        t.set_soloed(m->solo);
        t.set_solo_safe(m->solo_safe);
    }
    // The audio thread counts every engine track's solo: one left on a track
    // the document no longer has (an undone AddTrack) would silence the rest
    // while every replica shows no solo. Only the solo: the old plan renders
    // that track until the new one is resident and swapped in, so a fader or
    // mute reset here would be heard (a muted track at 0 dB), and after the
    // swap nothing renders it.
    for (std::size_t i = tracks.size(); i < session_.tracks().size(); ++i) {
        session_.tracks()[i]->set_soloed(false);
    }
    apply_monitoring(snapshot);
    session_.set_bus_gain(gain_of_db(snapshot.mix.bus.gain_db));
}

void PlaybackController::apply_gain(const model::Snapshot& snapshot, std::size_t track) {
    const auto& tracks = snapshot.edit->tracks;
    if (track >= tracks.size() || track >= session_.tracks().size()) return;
    const model::TrackMix* m = snapshot.mix.find(tracks[track]->id);
    if (m) session_.tracks()[track]->set_gain(gain_of_db(m->gain_db));
}

void PlaybackController::apply_monitoring(const model::Snapshot& snapshot) {
    const auto& tracks = snapshot.edit->tracks;
    for (std::size_t i = 0; i < tracks.size() && i < session_.tracks().size(); ++i) {
        const model::TrackMix* m = snapshot.mix.find(tracks[i]->id);
        if (!m) continue;
        Track& t = *session_.tracks()[i];
        if (tracks[i]->kind != model::TrackKind::Audio) {
            // Its engine track carries no audio of its own (ADR-0014 M-1):
            // a capture channel monitored or recorded through it would be
            // another source's sound under its name. Arming is fixed for a
            // take, so an arm left from before stays until the take ends,
            // and add_closed_take() leaves its file out.
            t.set_monitor(Track::Monitor::Off);
            if (session_.transport().state() != TransportState::Recording) {
                t.set_rec_armed(false);
            }
            continue;
        }
        t.set_monitor(!input_monitoring_ && t.rec_armed() ? Track::Monitor::Off
                                                          : monitor_of(m->monitor));
    }
}

bool PlaybackController::show(const model::Snapshot& snapshot, std::string& error) {
    // Before the plan, which can be refused: the next take's files name
    // their track and session by what the document says now either way.
    const auto& tracks = snapshot.edit->tracks;
    for (std::size_t i = 0; i < session_.tracks().size(); ++i) {
        session_.tracks()[i]->set_document_id(i < tracks.size() ? tracks[i]->id : 0);
    }
    session_.set_session_name(snapshot.settings.name);
    session_.set_session_id(snapshot.settings.session_id);
    // The roll loops over the document's loop range while loop playback is
    // on (ADR-0016, engine.transport.loop and update "the loop on switch").
    // One too short for the session to loop plays straight through, as no
    // range does.
    const auto& loop = snapshot.settings.transport.loop;
    if (!snapshot.settings.transport.loop_playback || loop[0] < 0 || loop[1] <= loop[0] ||
        !session_.set_loop(static_cast<uint64_t>(loop[0]), static_cast<uint64_t>(loop[1]))) {
        session_.set_loop(0, 0);
    }
    const model::Transport& tr = snapshot.settings.transport;
    pre_roll_ = static_cast<uint64_t>(std::max<model::Frames>(tr.pre_roll, 0));
    post_roll_ = static_cast<uint64_t>(std::max<model::Frames>(tr.post_roll, 0));
    session_.click().set_count_off_bars(tr.count_off_bars);
    PlaybackPlan* plan = nullptr;
    PlanReport report;
    const SourceSlot slot = [this](const model::Source& s, uint32_t& out, std::string& e) {
        return registered(s, out, e);
    };
    PlanContinuity continuity = playing_continuity_;
    if (!build_plan(snapshot, limits_, slot, resolver_, continuity,
                    session_.plans().next_epoch(), plan, report, error)) {
        return false;
    }
    // The slots are laid out in the builder's chunk size and read in the
    // reader's: two sizes would play the wrong frames. Both come from the
    // same session setting, so this is a wiring error, refused in every
    // build type rather than asserted in one.
    if (plan->chunk_frames != session_.reader().chunk_frames()) {
        destroy_plan(plan);
        error = "plan chunk size " + std::to_string(plan->chunk_frames) +
                " is not the reader's " + std::to_string(session_.reader().chunk_frames());
        return false;
    }
    report_ = report;
    last_built_epoch_ = plan->epoch;
    pending_continuity_ = std::move(continuity);
    PlaybackPlan* const cancelled = pending_;
    pending_ = plan;
    pending_gen_ = serve();
    // Never published, so no block can hold it: it goes as soon as the
    // reader has let go, without the epoch wait (ADR-0009 §4.2).
    if (cancelled) cancelled_.push_back({cancelled, pending_gen_});
    return true;
}

bool PlaybackController::locate(uint64_t frame, std::string& error) {
    if (session_.transport().state() == TransportState::Recording) {
        error = "locate during a take is not supported yet; stop first";
        return false;
    }
    locate_target_ = frame;
    // The transport counts locates applied, and applies every one sent, in
    // order: this one is applied at the count after the last one sent. A
    // request superseded before it is sent keeps the number.
    locate_serial_ = std::max(session_.transport().locates_applied(), sent_serial_) + 1;
    locate_sent_ = false;
    locate_asked_ = std::chrono::steady_clock::now();
    locate_gen_ = serve();
    return true;
}

void PlaybackController::play() {
    session_.set_roll_end(Session::kNoRollEnd);
    play_requested_ = true;
    play_gen_ = serve();
}

bool PlaybackController::play_range(uint64_t from, uint64_t to, std::string& error) {
    if (!locate(from > pre_roll_ ? from - pre_roll_ : 0, error)) return false;
    play();
    // After play(), which clears it. The roll starts only once the locate
    // has landed, so no block rolls toward this end from where it was.
    session_.set_roll_end(to + post_roll_);
    return true;
}

void PlaybackController::record(StartTake start) {
    start_take_ = std::move(start);
    record_requested_ = true;
    record_gen_ = serve();
}

std::string PlaybackController::take_record_error() {
    std::string e;
    e.swap(record_error_);
    return e;
}

void PlaybackController::stop() {
    play_requested_ = false;
    record_requested_ = false;
    session_.stop_playback();
}

// A roll starts once the window is resident and any locate before it has
// been applied, so the first block rolled plays the target.
bool PlaybackController::may_roll(uint64_t generation) const {
    return !pending_ && locate_sent_ && !session_.loop_pending() &&
           session_.reader().resident() >= generation &&
           session_.transport().locates_applied() >= locate_serial_;
}

void PlaybackController::pump() {
    DiskReader& reader = session_.reader();
    PlanExchange& plans = session_.plans();

    // A take is one contiguous span of the timeline: a locate not yet sent
    // when one starts is dropped, and the reader stops protecting its window.
    if (session_.transport().state() == TransportState::Recording && !locate_sent_) {
        locate_sent_ = true;
        locate_serial_ = sent_serial_;
        serve();
    }

    if (pending_) {
        // Rolling, the gate: publish only what is resident. Stopped, nothing
        // is heard, so it goes at once, and play() and record() wait for the
        // window (ADR-0009 §4.4, as amended).
        const bool stopped = session_.transport().state() == TransportState::Stopped;
        const bool ready = reader.resident() >= pending_gen_ ||
                           (stopped && reader.adopted() >= pending_gen_);
        if (ready) reclaim();
        if (ready && plans.can_publish()) {
            PlaybackPlan* const old = plans.current();
            plans.publish(pending_);
            pending_ = nullptr;
            playing_report_ = report_;
            playing_continuity_ = pending_continuity_;
            ++published_;
            const uint64_t gen = serve();
            if (old) retired_.push_back({old, gen});
            if (play_requested_) play_gen_ = gen;
            if (record_requested_) record_gen_ = gen;
            if (!locate_sent_) locate_gen_ = gen;
        }
    }

    if (!locate_sent_ && reader.resident() >= locate_gen_) {
        if (session_.post_locate(locate_target_)) {
            locate_sent_ = true;
            sent_serial_ = locate_serial_;
            last_locate_ms_ = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - locate_asked_)
                                  .count();
            if (last_locate_ms_ > max_locate_ms_) max_locate_ms_ = last_locate_ms_;
        }
    }

    session_.pump_loop();

    if (play_requested_ && may_roll(play_gen_)) {
        play_requested_ = false;
        session_.play();
    }
    if (record_requested_ && may_roll(record_gen_)) {
        record_requested_ = false;
        std::string why;
        if (!start_take_(why)) record_error_ = why.empty() ? "the take did not start" : why;
    }

    reclaim();
}

uint64_t PlaybackController::oldest_live_epoch() const {
    uint64_t oldest = UINT64_MAX;
    if (const PlaybackPlan* p = session_.plans().current()) oldest = std::min(oldest, p->epoch);
    if (pending_) oldest = std::min(oldest, pending_->epoch);
    for (const Dropped& d : retired_) oldest = std::min(oldest, d.plan->epoch);
    return oldest;
}

void PlaybackController::reclaim() {
    DiskReader& reader = session_.reader();
    for (std::size_t i = 0; i < cancelled_.size();) {
        if (reader.adopted() >= cancelled_[i].generation) {
            destroy_plan(cancelled_[i].plan);
            cancelled_[i] = cancelled_.back();
            cancelled_.pop_back();
        } else {
            ++i;
        }
    }
    const auto may_free = [](void* ctx, const PlaybackPlan* plan) {
        auto* self = static_cast<PlaybackController*>(ctx);
        for (const Dropped& d : self->retired_) {
            if (d.plan == plan) return self->session_.reader().adopted() >= d.generation;
        }
        return true;
    };
    session_.plans().reclaim(may_free, this);
    for (std::size_t i = 0; i < retired_.size();) {
        if (!session_.plans().is_retired(retired_[i].plan)) {
            retired_[i] = retired_.back();
            retired_.pop_back();
        } else {
            ++i;
        }
    }
}

bool PlaybackController::settle(double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(seconds);
    for (;;) {
        if (!session_.reader().running()) session_.reader().run_pass();
        pump();
        if (!pending_ && locate_sent_ && !play_requested_ && !record_requested_ &&
            !session_.loop_pending()) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void PlaybackController::after_deactivate() {
    DiskReader& reader = session_.reader();
    reader.after_deactivate();
    const uint64_t gen = serve();
    if (!reader.running()) reader.run_pass();
    for (int i = 0; i < 2000 && reader.adopted() < gen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto may_free = [](void* ctx, const PlaybackPlan* plan) {
        auto* self = static_cast<PlaybackController*>(ctx);
        for (const Dropped& d : self->retired_) {
            if (d.plan == plan) return self->session_.reader().adopted() >= d.generation;
        }
        return true;
    };
    session_.plans().reclaim_after_deactivate(may_free, this);
    reclaim();
}

}  // namespace sonora
