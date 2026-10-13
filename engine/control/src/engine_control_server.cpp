// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/engine_control_server.h"

#include <chrono>
#include <cmath>
#include <utility>

#include "sonora/api/wire.h"
#include "sonora/audio_backend.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"

namespace sonora {

namespace {

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

api::ModelUpdate to_wire(const model::ReplicationUpdate& u) {
    api::ModelUpdate m;
    switch (u.kind) {
        case model::ReplicationUpdate::Kind::Op: m.kind = api::ModelUpdateKind::Op; break;
        case model::ReplicationUpdate::Kind::Undo: m.kind = api::ModelUpdateKind::Undo; break;
        case model::ReplicationUpdate::Kind::Redo: m.kind = api::ModelUpdateKind::Redo; break;
        case model::ReplicationUpdate::Kind::Snapshot:
            m.kind = api::ModelUpdateKind::Snapshot;
            break;
    }
    m.revision = u.revision;
    m.undo_floor = u.undo_floor;
    m.next_id = u.next_id;
    m.bytes = u.bytes;
    return m;
}

// How long the control thread sleeps when nothing arrives: the period of
// the take-failure and fatal checks, as the terminal loop's frame was.
constexpr auto kIdle = std::chrono::milliseconds(50);

}  // namespace

EngineControlServer::EngineControlServer(Session& session, backend::AudioBackend* audio,
                                         Options options)
    : session_(session),
      audio_(audio),
      opts_(std::move(options)),
      block_(static_cast<uint32_t>(session.tracks().size())) {
    const auto n = static_cast<uint32_t>(session.tracks().size());
    writer_.init(block_.data(), block_.bytes(), n, n, opts_.publish_period_ns);
    publisher_ = std::make_unique<StatePublisher>(session_, audio_, writer_,
                                                  opts_.publish_period_ns);

    // From the folder, not from 1: systemd restarts the engine every time it
    // exits, and a counter that started over numbered the next take like the
    // last session's first one.
    take_ = session_.next_free_take();
    publisher_->set_take(take_);
    install_session_commands(registry_, session_, take_, quit_, opts_.may_record);
    for (const auto& spec : command_catalog()) {
        if (spec.domain == CommandDomain::Engine && registry_.implemented(spec.id)) {
            implemented_.push_back(spec.id);
        }
    }

    model::SessionSettings settings;
    settings.name = session_.config().name;
    settings.sample_rate = session_.config().sample_rate;
    settings.audio_format = session_.config().format == WavWriter::Format::Float32
                                ? model::AudioFormat::Float32
                                : model::AudioFormat::Int24;
    std::vector<std::string> names;
    for (const auto& t : session_.tracks()) names.push_back(t->name());
    document_ = std::make_unique<model::Document>(model::new_session_with_tracks(settings, names));
}

EngineControlServer::~EngineControlServer() { stop(); }

void EngineControlServer::post_event(const api::EngineEvent& event) {
    std::string bytes;
    api::wire::encode(event, bytes);
    if (!out_) {
        early_.push_back(std::move(bytes));
        return;
    }
    send(bytes);
}

void EngineControlServer::set_inputs_connected(uint32_t count, bool none_is_a_problem) {
    publisher_->set_inputs_connected(count);
    publisher_->set_health(api::kHealthNoInputs, count == 0 && none_is_a_problem);
}

void EngineControlServer::send(const std::string& bytes) {
    if (!out_) return;
    // Cannot fail for what the server sends: a Snapshot is trimmed to
    // kMaxModelBytes, under kMaxMessageBytes.
    std::vector<std::string> frames;
    std::string error;
    if (!api::wire::split_frames(bytes, frames, error)) return;
    for (std::string& f : frames) out_(std::move(f));
}

void EngineControlServer::start(Outbound out) {
    if (running()) return;
    out_ = std::move(out);

    api::Hello hello;
    hello.protocol_version = opts_.protocol_version;
    hello.state_layout_version = opts_.state_layout_version;
    hello.sample_rate = session_.config().sample_rate;
    hello.buffer_frames = audio_ != nullptr ? audio_->buffer_frames() : 0;
    hello.track_count = static_cast<uint32_t>(session_.tracks().size());
    for (const auto& t : session_.tracks()) hello.track_names.push_back(t->name());
    hello.implemented_commands = implemented_;
    std::string bytes;
    api::wire::encode(hello, bytes);
    send(bytes);

    // A replica starts from the whole document.
    model::ReplicationUpdate snap;
    std::string why;
    if (document_->snapshot(snap, why, api::wire::kMaxModelBytes)) {
        api::wire::encode(to_wire(snap), bytes);
        send(bytes);
    }
    for (const std::string& e : early_) send(e);
    early_.clear();

    // Something on the block before anyone reads it, so the first look at
    // the heartbeat is not "never published".
    publisher_->publish_once();
    publisher_->start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    next_housekeeping_ns_ = 0;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { run(); });
}

void EngineControlServer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        inbound_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    publisher_->stop();
    running_.store(false, std::memory_order_release);
}

void EngineControlServer::deliver(std::string frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !running()) return;
        inbound_.push_back(std::move(frame));
    }
    wake_.notify_one();
}

void EngineControlServer::run() {
    std::deque<std::string> batch;
    std::string message, error;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, kIdle, [this]() { return stopping_ || !inbound_.empty(); });
            if (stopping_) return;
            batch.swap(inbound_);
        }
        while (!batch.empty()) {
            // A frame out of place drops what was held for that message;
            // after M7 the connection is closed instead.
            if (reassembler_.add(batch.front(), message, error) ==
                api::wire::Reassembler::Result::Complete) {
                handle(message);
            }
            batch.pop_front();
        }
        tick();
    }
}

void EngineControlServer::reply(uint64_t seq, DispatchResult result,
                                const std::string& message) {
    api::CommandReply r;
    r.seq = seq;
    r.result = result;
    r.message = message;
    std::string bytes;
    api::wire::encode(r, bytes);
    send(bytes);
}

void EngineControlServer::handle(const std::string& bytes) {
    api::wire::Type type{};
    std::string error;
    // A message the codec refuses has no trustworthy seq to answer; it is
    // dropped. After M7 that is a client bug or a hostile peer, and the
    // engine must not fall over either way.
    if (!api::wire::peek_type(bytes, type)) return;

    if (type == api::wire::Type::CommandRequest) {
        api::CommandRequest r;
        if (!api::wire::decode(bytes, r, error)) return;
        if (find_command(r.command) == nullptr) {
            reply(r.seq, DispatchResult::UnknownCommand, "");
            return;
        }
        if (!is_engine_command(r.command)) {
            reply(r.seq, DispatchResult::NotImplemented,
                  r.command + " is the interface's own command; the engine does not run it");
            return;
        }
        CommandContext ctx;
        ctx.target = r.target;
        ctx.argument = r.argument;
        std::string why;
        const DispatchResult d = registry_.dispatch(r.command, ctx, why);
        publisher_->set_take(take_);
        reply(r.seq, d, why);
        return;
    }

    if (type == api::wire::Type::ParameterChange) {
        api::ParameterChange c;
        if (!api::wire::decode(bytes, c, error)) return;
        if (c.track >= session_.tracks().size()) {
            reply(c.seq, DispatchResult::Failed,
                  "there is no track " + std::to_string(c.track + 1));
            return;
        }
        // Written as an atomic the audio thread already reads; a NaN would
        // reach every sample of the track, so it is refused here.
        if (!(c.value >= 0.0f && c.value <= 4.0f)) {
            reply(c.seq, DispatchResult::Failed, "gain must be from 0 to 4 (linear)");
            return;
        }
        session_.tracks()[c.track]->set_gain(c.value);
        reply(c.seq, DispatchResult::Ok, "");
        return;
    }

    if (type == api::wire::Type::ModelRequest) {
        api::ModelRequest m;
        if (!api::wire::decode(bytes, m, error)) return;
        model::ReplicationUpdate u;
        std::string why;
        bool ok = false;
        switch (m.action) {
            case api::ModelAction::Apply: ok = document_->apply(m.op, u, why); break;
            case api::ModelAction::Undo: ok = document_->undo(u, why); break;
            case api::ModelAction::Redo: ok = document_->redo(u, why); break;
            case api::ModelAction::Resync:
                ok = document_->snapshot(u, why, api::wire::kMaxModelBytes);
                break;
        }
        reply(m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
        if (ok) {
            std::string out;
            api::wire::encode(to_wire(u), out);
            send(out);
        }
    }
}

void EngineControlServer::tick() {
    // The backend stopped calling the session. Said once; the interface
    // says why and leaves, and the wiring layer closes the take.
    if (audio_ != nullptr && !fatal_said_ && audio_->fatal()) {
        fatal_said_ = true;
        api::EngineEvent e;
        e.kind = api::EventKind::Fatal;
        e.text = audio_->fatal_reason();
        post_event(e);
    }

    // Writing stops at a full disk or the 4 GB limit while the transport
    // still says REC, so the failure is shown the moment the disk thread
    // records it, and until a later take records cleanly.
    if (session_.transport().state() == TransportState::Recording) {
        const std::string failure = session_.take_failure();
        if (!failure.empty() && failed_take_ != take_) {
            failed_take_ = take_;
            take_failed_shown_ = true;
            publisher_->set_health(api::kHealthTakeFailed, true);
            api::EngineEvent e;
            e.kind = api::EventKind::TakeFailed;
            e.frame = static_cast<uint64_t>(take_);
            e.text = "take " + std::to_string(take_) + " failed: " + failure +
                     ". Nothing more is being written; stop the take";
            post_event(e);
        } else if (failure.empty() && take_ != failed_take_ && take_failed_shown_) {
            take_failed_shown_ = false;
            publisher_->set_health(api::kHealthTakeFailed, false);
            api::EngineEvent e;
            e.kind = api::EventKind::TakeFailed;
            e.frame = static_cast<uint64_t>(take_);
            post_event(e);
        }
    }

    if (opts_.housekeeping) {
        const uint64_t now = steady_ns();
        if (now >= next_housekeeping_ns_) {
            next_housekeeping_ns_ = now + opts_.housekeeping_period_ns;
            opts_.housekeeping(*this);
        }
    }
}

}  // namespace sonora
