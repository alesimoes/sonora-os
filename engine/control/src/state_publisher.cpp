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
      tracks_(session.tracks().size()),
      peaks_(session.tracks().size(), 0.0f),
      clips_(session.tracks().size(), 0) {
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        input_connected_[i].store(false, std::memory_order_relaxed);
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
                  (input_connected_[i].load(std::memory_order_relaxed)
                       ? api::kTrackInputConnected
                       : 0u);
        f.gain = tr.gain();
        f.overruns = tr.overruns();
        f.clip_count = clips_[i];
        f.latency_frames = 0;   // no plugin delay exists yet; reported honestly
    }
    writer_.publish(t, tracks_.data(), peaks_.data());
    publishes_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace sonora
