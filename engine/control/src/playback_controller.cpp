// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/playback_controller.h"

#include <cmath>
#include <thread>

#include "sonora/session.h"

namespace sonora {
namespace {

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

float linear(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace

PlaybackController::PlaybackController(Session& session, std::string session_dir)
    : session_(session), dir_(std::move(session_dir)) {
    const PlaybackConfig& pb = session.config().playback;
    limits_.engine_tracks = static_cast<uint32_t>(session.tracks().size());
    limits_.channels_per_track = session.config().channels_per_track;
    limits_.max_segments = pb.max_segments;
    limits_.max_chunk_slots = pb.max_chunk_slots;
    limits_.chunk_frames = pb.chunk_frames;
    limits_.window_frames = session.window_frames();
    limits_.pool_chunks = session.reader().pool_chunks();
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
    const auto it = slots_.find(source.id);
    if (it != slots_.end()) {
        slot = it->second;
        return true;
    }
    // Sources are append-only in the model and in the engine's table, so a
    // slot, once given, is the source's for the session.
    if (!session_.reader().add_source(dir_ + "/" + source.path, slot, error)) return false;
    slots_.emplace(source.id, slot);
    return true;
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
        t.set_gain(linear(m->gain_db));
        t.set_pan(m->pan);
        t.set_muted(m->mute);
        t.set_soloed(m->solo);
        t.set_monitor(monitor_of(m->monitor));
    }
    session_.set_bus_gain(linear(snapshot.mix.bus.gain_db));
}

bool PlaybackController::show(const model::Snapshot& snapshot, std::string& error) {
    apply_mix(snapshot);
    PlaybackPlan* plan = nullptr;
    PlanReport report;
    const SourceSlot slot = [this](const model::Source& s, uint32_t& out, std::string& e) {
        return registered(s, out, e);
    };
    if (!build_plan(snapshot, limits_, slot, session_.plans().next_epoch(), plan, report, error)) {
        return false;
    }
    report_ = report;
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
    // The transport counts locates applied; this one is the next it will
    // apply. A request superseded before it is sent keeps the number.
    locate_serial_ = session_.transport().locates_applied() + 1;
    locate_sent_ = false;
    locate_asked_ = std::chrono::steady_clock::now();
    locate_gen_ = serve();
    return true;
}

void PlaybackController::play() {
    play_requested_ = true;
    play_gen_ = serve();
}

void PlaybackController::stop() {
    play_requested_ = false;
    session_.stop_playback();
}

void PlaybackController::pump() {
    DiskReader& reader = session_.reader();
    PlanExchange& plans = session_.plans();

    if (pending_) {
        // Rolling, the gate: publish only what is resident. Stopped, nothing
        // is heard, so it goes at once and play() waits for the window.
        const bool stopped = session_.transport().state() == TransportState::Stopped;
        const bool ready = reader.resident() >= pending_gen_ ||
                           (stopped && reader.adopted() >= pending_gen_);
        if (ready) reclaim();
        if (ready && plans.can_publish()) {
            PlaybackPlan* const old = plans.current();
            plans.publish(pending_);
            pending_ = nullptr;
            ++published_;
            const uint64_t gen = serve();
            if (old) retired_.push_back({old, gen});
            if (play_requested_) play_gen_ = gen;
            if (!locate_sent_) locate_gen_ = gen;
        }
    }

    if (!locate_sent_ && reader.resident() >= locate_gen_) {
        if (session_.post_locate(locate_target_)) {
            locate_sent_ = true;
            last_locate_ms_ = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - locate_asked_)
                                  .count();
            if (last_locate_ms_ > max_locate_ms_) max_locate_ms_ = last_locate_ms_;
        }
    }

    // A roll starts once the window is resident and any locate before it
    // has been applied, so the first block rolled plays the target.
    if (play_requested_ && !pending_ && locate_sent_ && reader.resident() >= play_gen_ &&
        session_.transport().locates_applied() >= locate_serial_) {
        play_requested_ = false;
        session_.play();
    }

    reclaim();
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
        if (!pending_ && locate_sent_ && !play_requested_) return true;
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
