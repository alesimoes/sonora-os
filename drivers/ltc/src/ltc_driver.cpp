// SPDX-License-Identifier: GPL-3.0-or-later
// LTC chase and generate drivers on the control thread (ADR-0124 §4, §5).
//
// They speak to the engine only through api::EngineControl: the published
// transport in, catalogue commands (transport.locate / play / stop) out.
#include <cstdint>
#include <string>
#include <utility>

#include "sonora/ltc/ltc_sync.h"

namespace sonora::ltc {
namespace {

// A transport the drivers may act on: read in one seqlock pass and not
// stale. A stale or unattached view is no information, so nothing moves.
bool read_transport(api::EngineControl& engine, api::TransportSnapshot& out) {
    api::StateView& view = engine.state();
    if (!view.attached()) return false;
    out = view.transport();
    return !out.stale;
}

void send_command(api::EngineControl& engine, const char* id, std::string argument = {}) {
    api::CommandRequest request;
    request.command = id;
    request.argument = std::move(argument);
    engine.send(request);
}

}  // namespace

// ---------------------------------------------------------------- chase

LtcChaseDriver::LtcChaseDriver(api::EngineControl& engine, LtcSampleSource& source,
                               const LtcConfig& config)
    : engine_(engine),
      source_(source),
      config_(config),
      decoder_(config.sample_rate, config.rate),
      chase_(config) {}

bool LtcChaseDriver::start(LtcRate session_rate, std::string& why) {
    started_ = validate_ltc_config(config_, LtcMode::Chase, session_rate, why);
    decoder_.reset();
    chase_.reset();
    return started_;
}

void LtcChaseDriver::tick() {
    if (!started_) return;
    api::TransportSnapshot transport;
    const bool known = read_transport(engine_, transport);
    if (known) chase_.observe(transport.playhead, transport.state);

    for (std::size_t chunk = 0; chunk < kMaxChunksPerTick; ++chunk) {
        const std::size_t n = source_.read(Span<float>{scratch_, kChunk});
        if (n == 0) break;
        // Decoded even when the transport is unknown, so the bit clock stays
        // in step and the next good tick acts on fresh frames, not old ones.
        decoder_.feed(Span<const float>{scratch_, n}, decoded_);
        if (!known) continue;
        for (std::size_t i = 0; i < decoded_.count; ++i) act(chase_.on_time(decoded_.time[i]));
    }
    // Without a transport to compare with, a silent tick is not counted
    // either: the chase must not stop a transport it cannot see.
    if (known) act(chase_.on_tick());
}

void LtcChaseDriver::act(ChaseAction action) {
    switch (action) {
    case ChaseAction::None:
        return;
    case ChaseAction::Locate: {
        // LtcChase never asks for a locate while recording; the state is read
        // again here because a take may have started since observe(), and a
        // take is never cut by a jump (ADR-0124 §4).
        api::TransportSnapshot now;
        if (!read_transport(engine_, now) || now.state == api::TransportState::Recording) return;
        send_command(engine_, "transport.locate", std::to_string(chase_.target()));
        break;
    }
    case ChaseAction::Play:
        send_command(engine_, "transport.play");
        break;
    case ChaseAction::Stop:
        send_command(engine_, "transport.stop");
        break;
    }
    ++commands_sent_;
}

// ---------------------------------------------------------------- generate

namespace {

// A playhead move bigger than this in one tick, or any move backwards, is a
// locate and restarts the timecode there. A quarter second is longer than a
// control tick stalls in practice and shorter than a deliberate locate.
std::uint64_t jump_limit(std::uint32_t sample_rate) { return sample_rate / 4u; }

}  // namespace

LtcGenerator::LtcGenerator(api::EngineControl& engine, LtcSampleSink& sink,
                           const LtcConfig& config)
    : engine_(engine),
      sink_(sink),
      config_(config),
      encoder_(config.sample_rate, config.rate) {
    next_.rate = config.rate;
}

bool LtcGenerator::start(LtcRate session_rate, std::string& why) {
    started_ = validate_ltc_config(config_, LtcMode::Generate, session_rate, why);
    encoder_.reset();
    next_ = LtcTime{};
    next_.rate = config_.rate;
    last_playhead_ = 0;
    rolling_ = false;
    return started_;
}

void LtcGenerator::tick() {
    if (!started_) return;
    api::TransportSnapshot transport;
    if (!read_transport(engine_, transport)) return;
    const std::uint64_t playhead = transport.playhead;
    const bool moving = transport.state != api::TransportState::Stopped;
    // Session sample = timecode sample + offset, so the timecode is behind
    // the playhead by the offset. Before timecode zero there is none to send.
    const std::int64_t timecode =
        static_cast<std::int64_t>(playhead) - config_.offset_samples;
    if (!moving || timecode < 0) {
        rolling_ = false;
        last_playhead_ = playhead;
        return;
    }

    const bool jumped = playhead < last_playhead_ ||
                        playhead - last_playhead_ > jump_limit(config_.sample_rate);
    if (!rolling_ || jumped) {
        // Restart at the frame the playhead is in, rendered from that frame's
        // edge and the part before the playhead thrown away, so the waveform
        // is in phase with the session from the first sample written.
        const RateRatio r = rate_ratio(config_.rate);
        const std::uint64_t tc = static_cast<std::uint64_t>(timecode);
        const std::uint64_t per_second = std::uint64_t{config_.sample_rate} * r.den;
        const std::uint64_t frame = tc * r.num / per_second;
        const std::uint64_t edge = (frame * per_second + r.num - 1u) / r.num;
        encoder_.reset();
        next_ = LtcTime::from_frames(frame, config_.rate);
        for (std::uint64_t skip = tc - edge; skip > 0;) {
            const std::size_t n = skip < kChunk ? static_cast<std::size_t>(skip) : kChunk;
            next_ = encoder_.render(next_, Span<float>{scratch_, n});
            skip -= n;
        }
        rolling_ = true;
        last_playhead_ = playhead;
        return;
    }

    for (std::uint64_t left = playhead - last_playhead_; left > 0;) {
        const std::size_t n = left < kChunk ? static_cast<std::size_t>(left) : kChunk;
        next_ = encoder_.render(next_, Span<float>{scratch_, n});
        // A short write is a full ring downstream: count it and go on, the
        // timecode keeps its place and the receiver re-locks.
        if (sink_.write(Span<const float>{scratch_, n}) < n) ++short_writes_;
        left -= n;
    }
    last_playhead_ = playhead;
}

}  // namespace sonora::ltc
