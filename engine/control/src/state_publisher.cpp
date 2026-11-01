// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/state_publisher.h"

#include <chrono>

#include "sonora/audio_backend.h"
#include "sonora/session.h"

namespace sonora {

StatePublisher::StatePublisher(Session& session, const backend::AudioBackend* audio,
                               api::StateWriter& writer, uint64_t period_ns)
    : session_(session),
      audio_(audio),
      writer_(writer),
      period_ns_(period_ns),
      input_connected_(new std::atomic<bool>[session.tracks().size()]),
      insert_flags_(new std::atomic<uint32_t>[session.tracks().size() * api::kInsertSlots]),
      insert_work_refused_(new std::atomic<uint32_t>[session.tracks().size() * api::kInsertSlots]),
      track_latency_(new std::atomic<uint32_t>[session.tracks().size()]),
      track_shortfall_(new std::atomic<uint32_t>[session.tracks().size()]),
      tracks_(session.tracks().size()),
      peaks_(session.tracks().size(), 0.0f),
      clips_(session.tracks().size(), 0) {
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        input_connected_[i].store(false, std::memory_order_relaxed);
        track_latency_[i].store(0, std::memory_order_relaxed);
        track_shortfall_[i].store(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < session.tracks().size() * api::kInsertSlots; ++i) {
        insert_flags_[i].store(0, std::memory_order_relaxed);
        insert_work_refused_[i].store(0, std::memory_order_relaxed);
    }
}

StatePublisher::~StatePublisher() { stop(); }

void StatePublisher::set_health(uint32_t flag, bool on) {
    if (on) {
        health_.fetch_or(flag, std::memory_order_relaxed);
    } else {
        health_.fetch_and(~flag, std::memory_order_relaxed);
    }
}

void StatePublisher::set_input_connected(uint32_t track, bool connected) {
    if (track < session_.tracks().size()) {
        input_connected_[track].store(connected, std::memory_order_relaxed);
    }
}

void StatePublisher::set_insert_flags(uint32_t track, uint32_t slot, uint32_t flags) {
    if (track < session_.tracks().size() && slot < api::kInsertSlots) {
        insert_flags_[track * api::kInsertSlots + slot].store(flags, std::memory_order_relaxed);
    }
}

void StatePublisher::set_insert_work_refused(uint32_t track, uint32_t slot, uint32_t count) {
    if (track < session_.tracks().size() && slot < api::kInsertSlots) {
        insert_work_refused_[track * api::kInsertSlots + slot].store(count, std::memory_order_relaxed);
    }
}

void StatePublisher::set_compensation(uint32_t frames, uint32_t shortfall_frames) {
    compensation_.store(frames, std::memory_order_relaxed);
    compensation_shortfall_.store(shortfall_frames, std::memory_order_relaxed);
}

void StatePublisher::set_track_latency(uint32_t track, uint32_t frames) {
    if (track < session_.tracks().size()) {
        track_latency_[track].store(frames, std::memory_order_relaxed);
    }
}

void StatePublisher::set_track_shortfall(uint32_t track, uint32_t shortfall_frames) {
    if (track < session_.tracks().size()) {
        track_shortfall_[track].store(shortfall_frames, std::memory_order_relaxed);
    }
}

void StatePublisher::start() {
    if (thread_.joinable()) return;
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this]() { run(); });
}

void StatePublisher::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

void StatePublisher::run() {
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(period_ns_);
    auto next = Clock::now();
    while (!stop_.load(std::memory_order_relaxed)) {
        publish_once();
        next += period;
        const auto now = Clock::now();
        // Late (the machine was busy, or the thread was descheduled): the
        // next publish is a period from now, not a burst of catch-up ones.
        if (next < now) next = now + period;
        std::this_thread::sleep_until(next);
    }
}

void StatePublisher::publish_once() {
    const auto& tracks = session_.tracks();
    Transport& transport = session_.transport();

    api::TransportFields t;
    switch (transport.state()) {
        case TransportState::Stopped: t.state = api::TransportState::Stopped; break;
        case TransportState::Rolling: t.state = api::TransportState::Rolling; break;
        case TransportState::Recording: t.state = api::TransportState::Recording; break;
    }
    t.playhead = transport.playhead();
    t.take = take_.load(std::memory_order_relaxed);
    t.overruns = session_.disk().total_overruns();
    t.health_flags = health_.load(std::memory_order_relaxed);
    t.inputs_connected = inputs_connected_.load(std::memory_order_relaxed);
    t.compensation_frames = compensation_.load(std::memory_order_relaxed);
    t.compensation_shortfall_frames = compensation_shortfall_.load(std::memory_order_relaxed);
    Click& click = session_.click();
    t.transport_flags = (click.on() ? api::kTransportClick : 0u) |
                        (click.count_off() ? api::kTransportCountOff : 0u) |
                        (click.counting_off() ? api::kTransportCountingOff : 0u) |
                        (locating_.load(std::memory_order_relaxed) ? api::kTransportLocating : 0u) |
                        (record_ready_.load(std::memory_order_relaxed) ? api::kTransportRecordReady
                                                                       : 0u);
    t.playback_misses = static_cast<uint32_t>(session_.playback_misses());
    const float bus[api::kBusPeakSides] = {session_.take_bus_peak(0), session_.take_bus_peak(1)};
    if (audio_ != nullptr) {
        t.xruns = audio_->xruns();
        t.clock_steps = audio_->clock_discontinuities();
        t.buffer_frames = audio_->buffer_frames();
        if (audio_->fatal()) t.health_flags |= api::kHealthServerGone;
    }

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        Track& tr = *tracks[i];
        const float peak = tr.take_peak();
        if (peak >= 1.0f) ++clips_[i];
        peaks_[i] = peak;
        api::TrackFields& f = tracks_[i];
        f.flags = (tr.rec_armed() ? api::kTrackArmed : 0u) |
                  (tr.muted() ? api::kTrackMuted : 0u) |
                  (tr.monitor() == Track::Monitor::Input ? api::kTrackInputMonitor : 0u) |
                  (input_connected_[i].load(std::memory_order_relaxed)
                       ? api::kTrackInputConnected
                       : 0u) |
                  (track_shortfall_[i].load(std::memory_order_relaxed) != 0
                       ? api::kTrackCompensationShort
                       : 0u);
        f.gain = tr.gain();
        f.overruns = tr.overruns();
        f.clip_count = clips_[i];
        f.latency_frames = track_latency_[i].load(std::memory_order_relaxed);
        for (uint32_t k = 0; k < api::kInsertSlots; ++k) {
            f.inserts[k].flags =
                insert_flags_[i * api::kInsertSlots + k].load(std::memory_order_relaxed);
            f.inserts[k].work_refused =
                insert_work_refused_[i * api::kInsertSlots + k].load(std::memory_order_relaxed);
        }
    }
    writer_.publish(t, tracks_.data(), peaks_.data(), bus);
    publishes_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace sonora
