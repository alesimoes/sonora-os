// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/fake_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "sonora/api/wire.h"
#include "sonora/model/replication.h"

namespace sonora::api::testing {

namespace {

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

constexpr double kPi = 3.14159265358979323846;

ModelUpdate to_wire(const model::ReplicationUpdate& u) {
    ModelUpdate m;
    switch (u.kind) {
        case model::ReplicationUpdate::Kind::Op: m.kind = ModelUpdateKind::Op; break;
        case model::ReplicationUpdate::Kind::Undo: m.kind = ModelUpdateKind::Undo; break;
        case model::ReplicationUpdate::Kind::Redo: m.kind = ModelUpdateKind::Redo; break;
        case model::ReplicationUpdate::Kind::Snapshot: m.kind = ModelUpdateKind::Snapshot; break;
    }
    m.revision = u.revision;
    m.undo_floor = u.undo_floor;
    m.next_id = u.next_id;
    m.bytes = u.bytes;
    return m;
}

}  // namespace

const std::vector<std::string>& FakeEngine::engine_commands() {
    // engine/control's install_session_commands(), less app.quit, which is
    // the interface's own (ADR-0007 §5). engine/control's test_control
    // fails if the two lists differ.
    static const std::vector<std::string> ids = {
        "track.arm_all",    "track.disarm_all", "track.toggle_arm",
        "transport.record", "transport.stop",   "transport.toggle_record",
    };
    return ids;
}

float FakeEngine::level_at(uint32_t ch, double t) {
    const double base_db = -42.0 + 2.0 * static_cast<double>(ch % 18);
    double db;
    if (ch < 4) {
        const double period = 0.45 + 0.1 * ch;
        const double phase = std::fmod(t + 0.07 * ch, period) / period;
        db = base_db + 24.0 * std::exp(-phase * 9.0) - 6.0;
    } else {
        db = base_db + 6.0 * std::sin(2.0 * kPi * (0.15 + 0.03 * ch) * t + ch);
    }
    if (ch == 9 && std::fmod(t, 7.0) < 0.05) db = 0.4;
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

FakeEngine::FakeEngine() : FakeEngine(Config{}) {}

FakeEngine::FakeEngine(Config config) : cfg_(std::move(config)), block_(cfg_.tracks) {
    if (!cfg_.clock) cfg_.clock = steady_ns;
    hello_.protocol_version = cfg_.protocol_version;
    hello_.state_layout_version = cfg_.state_layout_version;
    hello_.sample_rate = cfg_.sample_rate;
    hello_.buffer_frames = cfg_.buffer_frames;
    hello_.track_count = cfg_.tracks;
    for (uint32_t i = 0; i < cfg_.tracks; ++i) {
        hello_.track_names.push_back("Audio " + std::to_string(i + 1));
    }
    hello_.implemented_commands = engine_commands();
    armed_.assign(cfg_.tracks, false);
    gain_.assign(cfg_.tracks, 1.0f);
    scripted_.assign(cfg_.tracks, -1.0f);
    clips_.assign(cfg_.tracks, 0);

    model::SessionSettings settings;
    settings.name = "Untitled";
    settings.sample_rate = cfg_.sample_rate;
    document_ = std::make_unique<model::Document>(
        model::new_session_with_tracks(settings, hello_.track_names));
}

FakeEngine::~FakeEngine() = default;

uint64_t FakeEngine::now() const { return cfg_.clock(); }

bool FakeEngine::alive() const { return !dead_ && now() < death_ns_; }

bool FakeEngine::connect(std::string& error) {
    // The engine's half: the Hello goes out through the codec.
    std::string bytes;
    wire::encode(hello_, bytes);

    // The client's half: versions first, so a mismatch is named even if
    // the rest of a newer Hello no longer parses.
    uint32_t protocol = 0, layout = 0;
    if (!wire::decode_hello_versions(bytes, protocol, layout, error)) return false;
    error = wire::version_mismatch(protocol, layout);
    if (!error.empty()) return false;
    Hello decoded;
    if (!wire::decode(bytes, decoded, error)) return false;
    hello_ = std::move(decoded);

    if (!writer_.init(block_.data(), block_.bytes(), cfg_.tracks, cfg_.tracks,
                      cfg_.publish_period_ns)) {
        error = "the fake engine could not lay out its state block";
        return false;
    }
    start_ns_ = now();
    last_publish_ns_ = start_ns_;
    publish();
    if (!view_.attach(block_.data(), block_.bytes(), error)) return false;
    connected_ = true;

    // A replica starts from the whole document, as after M7.
    model::ReplicationUpdate snap;
    std::string why;
    if (document_->snapshot(snap, why, wire::kMaxModelBytes)) broadcast(to_wire(snap));
    return true;
}

void FakeEngine::queue(std::deque<std::string>& q, const std::string& message) {
    std::vector<std::string> frames;
    std::string error;
    if (!wire::split_frames(message, frames, error)) return;
    for (std::string& f : frames) q.push_back(std::move(f));
}

void FakeEngine::script_inputs(bool connected, const std::string& why) {
    if (connected == inputs_connected_) return;
    inputs_connected_ = connected;
    health_ = connected ? (health_ & ~kHealthNoInputs) : (health_ | kHealthNoInputs);
    EngineEvent e;
    e.kind = EventKind::InputsMissing;
    e.text = connected ? std::string() : (why.empty() ? "no capture input is connected" : why);
    post_event(e);
}

std::string FakeEngine::take_file(int32_t take) {
    return "Audio 1_" + std::string(take < 10 ? "0" : "") + std::to_string(take) + ".wav";
}

// The engine control thread's check, in the same words.
void FakeEngine::check_take() {
    if (mode_ != TransportState::Recording) return;
    if (fail_takes_ && failed_take_ != take_) {
        failed_take_ = take_;
        take_failed_shown_ = true;
        health_ |= kHealthTakeFailed;
        EngineEvent e;
        e.kind = EventKind::TakeFailed;
        e.frame = static_cast<uint64_t>(take_);
        e.text = "take " + std::to_string(take_) + " failed: " + take_file(take_) +
                 ": no space left on device. Nothing more is being written; stop the take";
        post_event(e);
    } else if (!fail_takes_ && take_ != failed_take_ && take_failed_shown_) {
        take_failed_shown_ = false;
        health_ &= ~kHealthTakeFailed;
        EngineEvent e;
        e.kind = EventKind::TakeFailed;
        e.frame = static_cast<uint64_t>(take_);
        post_event(e);
    }
}

uint64_t FakeEngine::send(const CommandRequest& request) {
    CommandRequest r = request;
    r.seq = ++seq_;
    std::string bytes;
    wire::encode(r, bytes);
    queue(inbound_, bytes);
    return r.seq;
}

uint64_t FakeEngine::send(const ParameterChange& change) {
    ParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    wire::encode(c, bytes);
    queue(inbound_, bytes);
    return c.seq;
}

uint64_t FakeEngine::submit(const ModelRequest& request) {
    ModelRequest m = request;
    m.seq = ++seq_;
    std::string bytes;
    wire::encode(m, bytes);
    queue(inbound_, bytes);
    return m.seq;
}

void FakeEngine::post_event(const EngineEvent& event) {
    std::string bytes;
    wire::encode(event, bytes);
    queue(outbound_, bytes);
}

void FakeEngine::script_peak(uint32_t track, float peak) {
    if (track < scripted_.size()) scripted_[track] = peak;
}

void FakeEngine::reply(uint64_t seq, DispatchResult result, std::string message) {
    CommandReply r;
    r.seq = seq;
    r.result = result;
    r.message = std::move(message);
    std::string bytes;
    wire::encode(r, bytes);
    queue(outbound_, bytes);
}

void FakeEngine::broadcast(const ModelUpdate& u) {
    std::string bytes;
    wire::encode(u, bytes);
    queue(outbound_, bytes);
}

void FakeEngine::poll(Inbox& inbox) {
    if (!connected_ || !alive()) {
        // A dead engine's requests go nowhere, and nothing comes back.
        inbound_.clear();
        return;
    }
    std::string bytes, error;
    while (!inbound_.empty()) {
        const std::string frame = std::move(inbound_.front());
        inbound_.pop_front();
        if (engine_side_.add(frame, bytes, error) == wire::Reassembler::Result::Complete) {
            handle(bytes);
        }
    }
    check_take();
    publish();

    // The client's half: everything back through the codec.
    while (!outbound_.empty()) {
        const std::string frame = std::move(outbound_.front());
        outbound_.pop_front();
        if (client_side_.add(frame, bytes, error) != wire::Reassembler::Result::Complete) {
            continue;
        }
        wire::Type type{};
        if (!wire::peek_type(bytes, type)) continue;
        if (type == wire::Type::CommandReply) {
            CommandReply r;
            if (wire::decode(bytes, r, error)) inbox.replies.push_back(std::move(r));
        } else if (type == wire::Type::EngineEvent) {
            EngineEvent e;
            if (wire::decode(bytes, e, error)) inbox.events.push_back(std::move(e));
        } else if (type == wire::Type::ModelUpdate) {
            ModelUpdate m;
            if (wire::decode(bytes, m, error)) inbox.model.push_back(std::move(m));
        }
    }
}

void FakeEngine::handle(const std::string& bytes) {
    wire::Type type{};
    std::string error;
    if (!wire::peek_type(bytes, type)) return;
    if (type == wire::Type::CommandRequest) {
        CommandRequest r;
        if (!wire::decode(bytes, r, error)) return;
        if (find_command(r.command) == nullptr) {
            reply(r.seq, DispatchResult::UnknownCommand, "");
            return;
        }
        if (!is_engine_command(r.command)) {
            reply(r.seq, DispatchResult::NotImplemented,
                  r.command + " is the interface's own command; the engine does not run it");
            return;
        }
        const auto& ids = engine_commands();
        if (std::find(ids.begin(), ids.end(), r.command) == ids.end()) {
            reply(r.seq, DispatchResult::NotImplemented, "");
            return;
        }
        std::string why;
        const bool ok = command(r, why);
        reply(r.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
    } else if (type == wire::Type::ParameterChange) {
        ParameterChange c;
        if (!wire::decode(bytes, c, error)) return;
        if (c.track >= cfg_.tracks) {
            reply(c.seq, DispatchResult::Failed,
                  "there is no track " + std::to_string(c.track + 1));
        } else if (!(c.value >= 0.0f && c.value <= 4.0f)) {
            reply(c.seq, DispatchResult::Failed, "gain must be from 0 to 4 (linear)");
        } else {
            gain_[c.track] = c.value;
            reply(c.seq, DispatchResult::Ok, "");
        }
    } else if (type == wire::Type::ModelRequest) {
        ModelRequest m;
        if (!wire::decode(bytes, m, error)) return;
        model::ReplicationUpdate u;
        std::string why;
        bool ok = false;
        switch (m.action) {
            case ModelAction::Apply: ok = document_->apply(m.op, u, why); break;
            case ModelAction::Undo: ok = document_->undo(u, why); break;
            case ModelAction::Redo: ok = document_->redo(u, why); break;
            case ModelAction::Resync:
                ok = document_->snapshot(u, why, wire::kMaxModelBytes);
                break;
            case ModelAction::Load:
                ok = document_->load(m.op, cfg_.sample_rate, u, why, wire::kMaxModelBytes);
                break;
        }
        reply(m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
        if (ok) broadcast(to_wire(u));
    }
}

// The same handlers, refusals and words as engine/control's
// install_session_commands(); the conformance suite holds both to them.
bool FakeEngine::command(const CommandRequest& r, std::string& why) {
    const bool recording = mode_ == TransportState::Recording;
    const auto start = [&]() {
        if (std::find(armed_.begin(), armed_.end(), true) == armed_.end()) {
            why = "no track is armed";
            return false;
        }
        mode_ = TransportState::Recording;
        record_start_ns_ = now();
        return true;
    };
    // Failed reports the take, not the stop, as the real one does.
    const auto finish = [&]() {
        mode_ = TransportState::Stopped;
        const bool failed = failed_take_ == take_ && take_failed_shown_;
        if (failed) why = take_file(take_) + ": no space left on device";
        ++take_;
        return !failed;
    };
    const std::string& id = r.command;
    if (id == "transport.toggle_record") return recording ? finish() : start();
    if (id == "transport.record") return recording ? true : start();
    if (id == "transport.stop") return mode_ == TransportState::Stopped ? true : finish();

    const auto take_running = [&]() {
        if (recording) why = "arming cannot change during a take; stop first";
        return recording;
    };
    if (id == "track.toggle_arm") {
        if (r.target < 0) {
            why = "no track given, and there is no selection yet";
            return false;
        }
        const auto idx = static_cast<std::size_t>(r.target);
        if (idx >= armed_.size()) {
            why = "there is no track " + std::to_string(idx + 1);
            return false;
        }
        if (take_running()) return false;
        armed_[idx] = !armed_[idx];
        return true;
    }
    if (id == "track.arm_all" || id == "track.disarm_all") {
        if (take_running()) return false;
        std::fill(armed_.begin(), armed_.end(), id == "track.arm_all");
        return true;
    }
    why = "not handled";
    return false;
}

void FakeEngine::publish() {
    if (!alive()) return;
    const uint64_t t1 = now();
    const uint64_t t0 = std::max(last_publish_ns_, t1 >= 1'000'000'000 ? t1 - 1'000'000'000 : 0);
    last_publish_ns_ = t1;

    TransportFields tf;
    tf.state = mode_;
    tf.take = take_;
    if (mode_ == TransportState::Recording && t1 >= record_start_ns_) {
        tf.playhead = (t1 - record_start_ns_) * cfg_.sample_rate / 1'000'000'000ull;
    }
    tf.xruns = xruns_;
    tf.clock_steps = clock_steps_;
    tf.health_flags = health_;
    tf.inputs_connected = inputs_connected_ ? cfg_.tracks : 0;
    tf.buffer_frames = cfg_.buffer_frames;

    std::vector<TrackFields> tracks(cfg_.tracks);
    std::vector<float> peaks(cfg_.tracks, 0.0f);
    const double a = static_cast<double>(t0 - std::min(t0, start_ns_)) * 1e-9;
    const double b = static_cast<double>(t1 - std::min(t1, start_ns_)) * 1e-9;
    for (uint32_t i = 0; i < cfg_.tracks; ++i) {
        float peak = 0.0f;
        if (scripted_[i] >= 0.0f) {
            peak = scripted_[i];
        } else if (cfg_.signal) {
            // The engine's peak is the maximum of every sample since the
            // last publish; the envelope sampled each millisecond is close
            // enough, and a transient between two publishes still shows.
            for (double t = a; t <= b; t += 0.001) peak = std::max(peak, level_at(i, t));
            peak = std::max(peak, level_at(i, b));
        }
        if (peak >= 1.0f) ++clips_[i];
        peaks[i] = peak;
        tracks[i].flags =
            (armed_[i] ? kTrackArmed : 0u) | (inputs_connected_ ? kTrackInputConnected : 0u);
        tracks[i].gain = gain_[i];
        tracks[i].clip_count = clips_[i];
    }
    writer_.publish(tf, tracks.data(), peaks.data());
}

}  // namespace sonora::api::testing
