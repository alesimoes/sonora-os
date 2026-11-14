// SPDX-License-Identifier: GPL-3.0-or-later
// MTC chase and generation (ADR-0122), on the control thread. The chase
// drives ChaseRule itself rather than through MtcChase so that it can hold
// back a locate while the engine records: MtcChase would send it.
#include "sonora/surfaces/mtc_sync.h"

#include <string>

namespace sonora::surfaces {

namespace {

// One command per tick at most: the published transport does not move
// between the positions of one poll, so a second command would act on the
// same stale state. A locate outranks the rest; it carries the last target.
int rank(ChaseAction a) {
    switch (a) {
        case ChaseAction::None: return 0;
        case ChaseAction::Play: return 1;
        case ChaseAction::Stop: return 2;
        case ChaseAction::Locate: return 3;
    }
    return 0;
}

std::uint64_t distance(std::int64_t a, std::int64_t b) {
    return a > b ? static_cast<std::uint64_t>(a - b) : static_cast<std::uint64_t>(b - a);
}

}  // namespace

// --- Chase -----------------------------------------------------------------

MtcChaseDriver::MtcChaseDriver(const MtcSyncSettings& settings, api::EngineControl& engine,
                               MidiPort& port)
    : settings_(settings), engine_(engine), port_(port), rule_(settings.sample_rate) {}

bool MtcChaseDriver::start(std::string& error) {
    error = validate_mtc_settings(settings_, MtcMode::Chase);
    started_ = error.empty();
    decoder_.reset();
    rule_ = ChaseRule(settings_.sample_rate);
    stats_ = MtcSyncStats{};
    silent_samples_ = 0;
    silent_frames_ = 0;
    return started_;
}

ChaseSnapshot MtcChaseDriver::to_rule(const api::TransportSnapshot& t) const {
    // The rule works in timecode samples; the engine's playhead is moved
    // into that frame of reference, not the master's position out of it.
    const std::int64_t tc = static_cast<std::int64_t>(t.playhead) - settings_.offset_samples;
    return ChaseSnapshot{t.state != api::TransportState::Stopped,
                         tc < 0 ? 0u : static_cast<std::uint64_t>(tc)};
}

ChaseAction MtcChaseDriver::decide(const Timecode& tc, const api::TransportSnapshot& t) {
    if (tc.rate != settings_.rate) ++stats_.rate_mismatches;
    return rule_.on_position(tc, to_rule(t));
}

void MtcChaseDriver::send(const char* command, const std::string& argument) {
    api::CommandRequest r;
    r.command = command;
    r.argument = argument;
    engine_.send(r);
}

void MtcChaseDriver::act(ChaseAction a, const api::TransportSnapshot& t) {
    switch (a) {
        case ChaseAction::None: return;
        case ChaseAction::Locate: {
            const std::int64_t target =
                static_cast<std::int64_t>(rule_.target()) + settings_.offset_samples;
            if (t.state == api::TransportState::Recording) {
                ++stats_.drift_events;
                const std::uint64_t d = distance(static_cast<std::int64_t>(t.playhead), target);
                if (d > stats_.max_drift_samples) stats_.max_drift_samples = d;
                return;
            }
            if (target < 0) {
                ++stats_.out_of_range;
                return;
            }
            send("transport.locate", std::to_string(target));
            ++stats_.locates;
            return;
        }
        case ChaseAction::Play:
            send("transport.play", {});
            ++stats_.plays;
            return;
        case ChaseAction::Stop:
            send("transport.stop", {});
            ++stats_.stops;
            return;
    }
}

void MtcChaseDriver::tick(std::uint64_t elapsed_samples) {
    if (!started_) return;
    const api::TransportSnapshot t = engine_.state().transport();
    bool heard = false;
    ChaseAction best = ChaseAction::None;
    decoder_.poll(port_, [&](const MtcEvent& e) {
        heard = true;
        const ChaseAction a = decide(e.position, t);
        if (rank(a) >= rank(best)) best = a;
    });
    if (heard) {
        silent_samples_ = 0;
        silent_frames_ = 0;
        act(best, t);
        return;
    }
    // Silence in whole frame periods on the session's rate; the remainder
    // carries over so a tick shorter than a frame still counts.
    silent_samples_ += elapsed_samples;
    const RateRatio r = rate_ratio(settings_.rate);
    const std::uint64_t frames =
        silent_samples_ * r.num / (std::uint64_t{settings_.sample_rate} * r.den);
    const std::uint64_t fresh = frames - silent_frames_;
    if (fresh == 0) return;
    silent_frames_ = frames;
    const ChaseAction a = rule_.tick(static_cast<std::uint32_t>(fresh), to_rule(t));
    act(a, t);
    if (a == ChaseAction::Stop) decoder_.reset();
}

// --- Generate --------------------------------------------------------------

MtcGenerator::MtcGenerator(const MtcSyncSettings& settings, api::EngineControl& engine,
                           MidiPort& port)
    : settings_(settings), engine_(engine), port_(port) {}

bool MtcGenerator::start(std::string& error) {
    error = validate_mtc_settings(settings_, MtcMode::Generate);
    started_ = error.empty();
    stats_ = MtcSyncStats{};
    have_last_ = false;
    rolling_ = false;
    full_frame_pending_ = false;
    next_quarter_ = 0;
    last_tc_samples_ = 0;
    return started_;
}

std::int64_t MtcGenerator::timecode_samples(std::uint64_t playhead) const {
    return static_cast<std::int64_t>(playhead) - settings_.offset_samples;
}

void MtcGenerator::full_frame_at(std::int64_t tc_samples) {
    const Timecode tc = timecode_at(tc_samples, settings_.rate, settings_.sample_rate);
    full_frame_pending_ = !MtcEncoder::send_full_frame(port_, tc);
    if (full_frame_pending_) {
        ++stats_.refused_writes;
    } else {
        ++stats_.full_frames;
    }
}

// A cycle starts with piece 0 at the start of an even frame. Rolling
// begins at the next such boundary, not the current cycle's, so the first
// tick never bursts out pieces that are already late.
void MtcGenerator::begin_rolling(std::int64_t q_now) {
    std::int64_t cycle = q_now >= 0 ? q_now / 8 : -((-q_now + 7) / 8);
    if (cycle * 8 != q_now) ++cycle;
    encoder_.start(from_frames(cycle * 2, settings_.rate));
    next_quarter_ = cycle * 8;
}

bool MtcGenerator::jumped(std::int64_t q_now) const {
    return q_now - next_quarter_ >= kMaxCatchUpQuarters || next_quarter_ - q_now > 8;
}

void MtcGenerator::roll(std::int64_t q_now) {
    while (next_quarter_ <= q_now) {
        if (encoder_.send_next(port_)) {
            ++stats_.quarter_frames;
        } else {
            ++stats_.refused_writes;  // a late quarter frame is worse than none
        }
        ++next_quarter_;
    }
}

void MtcGenerator::tick(std::uint64_t /*elapsed_samples*/) {
    if (!started_) return;
    const api::TransportSnapshot t = engine_.state().transport();
    const bool rolling = t.state != api::TransportState::Stopped;
    const std::int64_t tc_s = timecode_samples(t.playhead);
    const std::int64_t q_now = quarter_frame_index(tc_s, settings_.rate, settings_.sample_rate);
    const std::int64_t q_last =
        quarter_frame_index(last_tc_samples_, settings_.rate, settings_.sample_rate);

    // Where we are, for every receiver that joins now: on start, a stop, a
    // locate (stopped, or a jump on starting to roll), or a refused retry.
    bool full = !have_last_ || full_frame_pending_;
    if (rolling_ && !rolling) full = true;
    if (!rolling && !rolling_ && have_last_ && tc_s != last_tc_samples_) full = true;
    if (rolling && !rolling_ && have_last_ &&
        (q_now - q_last > kMaxCatchUpQuarters || q_last - q_now > kMaxCatchUpQuarters)) {
        full = true;
    }
    if (rolling && rolling_ && jumped(q_now)) {
        ++stats_.resyncs;
        full = true;
        begin_rolling(q_now);
    }
    if (full) full_frame_at(tc_s);
    if (rolling && !rolling_) begin_rolling(q_now);
    if (rolling) roll(q_now);

    rolling_ = rolling;
    have_last_ = true;
    last_tc_samples_ = tc_s;
}

std::unique_ptr<MtcSync> make_mtc_sync(const MtcSyncSettings& settings, api::EngineControl& engine,
                                       MidiPort& port) {
    switch (settings.mode) {
        case MtcMode::Chase: return std::make_unique<MtcChaseDriver>(settings, engine, port);
        case MtcMode::Generate: return std::make_unique<MtcGenerator>(settings, engine, port);
    }
    return nullptr;
}

}  // namespace sonora::surfaces
