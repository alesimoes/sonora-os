// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's side of the control API (ADR-0007 §4.1): where engine/core
// and contracts/api meet, and nowhere else.
//
// It owns the state block's writer and its publisher, the command handlers
// (install_session_commands), and the session document (ADR-0009 §2), and
// runs a control thread that takes requests as encoded bytes, handles them
// in order and answers as encoded bytes. It knows no transport: each client
// is an Outbound callback, attached with attach() and fed with deliver().
// In process that is InProcessEngine's queue; across processes it is
// ControlSocketServer (M7), one client per connection, and the state block
// lives in the memory Options::state_memory names (a memfd).
//
// Any number of clients. A reply goes to the client that asked; events and
// model updates go to every client. A client that attaches late gets Hello,
// the document's snapshot (the undo window included), and the events that
// still stand: notices, missing inputs, a failed take, a fatal backend. A
// client that frames or encodes wrongly is refused and detached, and nothing
// it sent reaches the session.
//
// Playback (ADR-0009, update "playback on the M4/M7 control server"): the
// control thread is the document thread. It owns the PlaybackController,
// shows it the document after every change, adds each closed take to the
// document and sends the replicas the operations, and pumps the controller
// every turn of its loop, every 2 ms while something waits on residency.
// A take asked for by transport.record or transport.toggle_record is
// answered only once the gate resolves: Ok when it has started, Failed with
// the reason when it was refused, Failed "cancelled" when it was withdrawn.
//
// Nothing here runs on the audio thread. The handlers reach it only
// through the atomics Track and Transport already have, the plan exchange
// and the session's command ring; the publisher reads those atomics on its
// own thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/messages.h"
#include "sonora/api/state_block.h"
#include "sonora/api/wire.h"
#include "sonora/command.h"
#include "sonora/session_commands.h"
#include "sonora/state_publisher.h"

namespace sonora {

class PlaybackController;
class Session;
namespace backend {
class AudioBackend;
}
namespace model {
class Document;
struct ReplicationUpdate;
struct Snapshot;
}

class EngineControlServer {
public:
    using Outbound = std::function<void(std::string bytes)>;
    // Called on the control thread when a client is dropped for what it
    // sent; the transport then closes the connection. `why` names the fault.
    using Refuse = std::function<void(const std::string& why)>;
    using ClientId = uint64_t;

    struct Options {
        // 100 Hz: inside the 100 Hz - 1 kHz the measurement found clean,
        // and the meters' ballistics need no more.
        uint64_t publish_period_ns = 10'000'000;
        // Asked before every take (session_commands.h).
        MayRecord may_record;
        // What the wiring layer keeps an eye on (inputs, the device
        // fallback), run on the control thread every housekeeping period.
        // It may call post_event() and the input setters.
        std::function<void(EngineControlServer&)> housekeeping;
        uint64_t housekeeping_period_ns = 1'000'000'000;
        // What Hello announces. Only the conformance suite changes these,
        // to check that a client refuses a mismatch.
        uint32_t protocol_version = api::kProtocolVersion;
        uint32_t state_layout_version = api::kStateLayoutVersion;
        // Where the state block lives: at least api::state_block_bytes() for
        // the session's tracks, 64-byte aligned, outliving the server. Null:
        // anonymous memory of the server's own.
        void* state_memory = nullptr;
        std::size_t state_memory_bytes = 0;
        // What one client may have delivered and not yet handled. Past it,
        // wait_for_room() holds that client's reader back, so a client that
        // floods is stopped at its own socket and costs bounded memory.
        std::size_t max_client_backlog_bytes = std::size_t{1} << 20;
        // A message over `large_message_bytes` (a Load, mostly) is held
        // whole while it is reassembled. At most this many are reassembled
        // at once; another client's large message waits, frames and all,
        // until one completes, and its reader waits with it.
        std::size_t large_message_bytes = std::size_t{1} << 20;
        std::size_t max_large_in_flight = 2;
        // The document the server starts from (what --place put on the
        // timeline). Null: engine_document(session), empty tracks.
        std::shared_ptr<const model::Snapshot> initial_document;
        // False: an armed track sends nothing to the monitor bus, whatever
        // its mode in the document (--no-input-monitoring).
        bool input_monitoring = true;
    };

    static constexpr ClientId kFirstClient = 1;

    // `audio` may be null: then there are no xrun counts, no fatal check,
    // and buffer_frames is 0. Sizes the state block for the session's
    // tracks, here and once.
    EngineControlServer(Session& session, backend::AudioBackend* audio, Options options);
    ~EngineControlServer();
    EngineControlServer(const EngineControlServer&) = delete;
    EngineControlServer& operator=(const EngineControlServer&) = delete;

    // Starts the publisher and the control thread, with no client.
    void start();
    // As start(), with `out` attached first as kFirstClient: its Hello,
    // snapshot and standing events have gone through `out` when this
    // returns. `out` is called on the control thread from then on.
    void start(Outbound out);
    // Joins both threads and drops every client. Idempotent. Requests
    // delivered afterwards are dropped: a stopped engine answers nothing.
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // A new client. Any thread; never waits. Its Hello, snapshot and
    // standing events go through `out`, on the control thread, before
    // anything else does. `refuse` may be empty.
    ClientId attach(Outbound out, Refuse refuse);
    // The client is gone; nothing more is sent to it. Any thread.
    void detach(ClientId client);
    // One frame of a client's request (api/wire.h). Any thread; never
    // waits for the request to be handled. Outbound gets frames too.
    void deliver(ClientId client, std::string frame);
    void deliver(std::string frame) { deliver(kFirstClient, std::move(frame)); }

    enum class Room { Ok, Full, Gone };
    // For a transport's reader, before it reads the client's next frame:
    // waits up to `timeout_ms` while that client's backlog is full. Gone:
    // the client was detached or refused, or the server stopped. Any thread
    // but the control thread, which never waits on a client.
    Room wait_for_room(ClientId client, int timeout_ms);

    const void* state_block() const { return block_; }
    std::size_t state_block_bytes() const { return block_bytes_; }
    // Whether the block is where it should be. False only when
    // Options::state_memory was too small or misaligned.
    bool state_ready() const { return state_ready_; }

    // Clients attached now, and clients refused for what they sent since
    // construction. For diagnostics and the tests.
    std::size_t clients() const { return client_count_.load(std::memory_order_acquire); }
    uint64_t refused() const { return refused_.load(std::memory_order_acquire); }
    // The most one client has had delivered and not handled, and the most
    // large messages reassembled at once, since construction.
    std::size_t peak_backlog_bytes() const {
        return peak_backlog_.load(std::memory_order_acquire);
    }
    std::size_t peak_large_in_flight() const {
        return peak_large_.load(std::memory_order_acquire);
    }

    // --- the control thread, housekeeping, or before start() ---
    void post_event(const api::EngineEvent& event);
    void set_input_connected(uint32_t track, bool connected) {
        publisher_->set_input_connected(track, connected);
    }
    void set_inputs_connected(uint32_t count, bool none_is_a_problem);

    // Engine-domain commands with a handler, in catalog order.
    const std::vector<std::string>& implemented() const { return implemented_; }

    // After stop() and AudioBackend::deactivate() have both returned: frees
    // every retired plan (ADR-0009 §4.1: then, and only then).
    void after_deactivate();

private:
    struct Client {
        Outbound out;
        Refuse refuse;
        api::wire::Reassembler reassembler;
        // Frames held back behind a large message that has no slot yet.
        std::deque<std::string> parked;
    };
    struct Inbound {
        enum class Kind { Attach, Frame, Detach };
        Kind kind = Kind::Frame;
        ClientId client = 0;
        std::string bytes;
        Outbound out;
        Refuse refuse;
    };

    void start_threads();
    void run();
    void take(Inbound& in);
    // One frame into the client's reassembler, or parked behind the cap on
    // large messages.
    void frame(ClientId id, std::string bytes);
    void feed(ClientId id, Client& c, const std::string& bytes);
    // Parked clients get a slot as slots free up, oldest first.
    void unpark();
    void forget(ClientId id);
    // Handled frames' bytes leave their clients' backlogs.
    void release_handled();
    void greet(Client& client);
    void refuse(ClientId id, const std::string& why);
    void handle(ClientId from, const std::string& bytes);
    void tick();
    // To every client.
    void send(const std::string& bytes);
    void send_to(Client& client, const std::string& bytes);
    void send_to(ClientId id, const std::string& bytes);
    void reply(ClientId to, uint64_t seq, DispatchResult result, const std::string& message);
    void remember(const api::EngineEvent& event, const std::string& bytes);
    void send_update(const model::ReplicationUpdate& update);
    // The document changed: its plan to the controller, and its mix to the
    // Track atomics only when `mix_changed` (see show()).
    void show_document(bool mix_changed);
    // The document's inserts to the state block, every one faulted until
    // this engine can host plugins, and one standing PluginFault per active
    // insert: said when it enters the chain, cleared when it leaves (removed,
    // inactive). With `new_session`, every standing fault is cleared first:
    // the ids are another session's.
    void show_inserts(bool new_session);
    // A FlushPluginState: which inserts, and whether they can be flushed.
    bool flush_plugin_state(const std::string& which, std::string& why);
    // A take that has just closed joins the document.
    void follow_transport();
    // Held record replies, once the controller no longer waits to start.
    void resolve_records();
    void pump_playback();
    bool playback_busy() const;

    Session& session_;
    backend::AudioBackend* audio_;
    Options opts_;

    api::StateBlockMemory own_block_;   // when Options::state_memory is null
    void* block_ = nullptr;
    std::size_t block_bytes_ = 0;
    bool state_ready_ = false;
    api::StateWriter writer_;
    std::unique_ptr<StatePublisher> publisher_;
    std::unique_ptr<model::Document> document_;
    // Built after document_ and destroyed before it; touched only by the
    // control thread, or before start() and after stop().
    std::unique_ptr<PlaybackController> playback_;

    CommandRegistry registry_;
    std::vector<std::string> implemented_;
    // The take install_session_commands numbers. The control thread's own;
    // the publisher sees it through set_take().
    int take_ = 1;
    bool quit_ = false;   // app.quit's flag; never set, as the engine never runs it

    // The control thread's, or start()'s before that thread exists.
    std::map<ClientId, Client> clients_;
    // Events that still stand, encoded, for a client that attaches later:
    // what a restarted interface must find on screen.
    std::vector<std::string> notices_;
    // One per insert with a fault standing, by insert id: kept apart from
    // notices_ so a cleared fault is never replayed and faults never crowd
    // a device notice out of kMaxStandingNotices.
    std::map<uint64_t, std::string> plugin_faults_;
    std::string inputs_missing_;
    std::string take_failed_;
    std::string fatal_;

    // The control thread's: clients reassembling a large message, clients
    // waiting for a slot (in arrival order), and bytes handled since the
    // backlogs were last brought down.
    std::set<ClientId> large_;
    std::deque<ClientId> parked_;
    std::map<ClientId, std::size_t> handled_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable room_;
    std::deque<Inbound> inbound_;
    // Bytes delivered and not yet handled, per client attached.
    std::map<ClientId, std::size_t> backlog_;
    std::atomic<std::size_t> peak_backlog_{0};
    std::atomic<std::size_t> peak_large_{0};
    ClientId next_client_ = kFirstClient + 1;
    bool stopping_ = false;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> client_count_{0};
    std::atomic<uint64_t> refused_{0};
    std::thread thread_;

    struct HeldReply {
        ClientId to = 0;
        uint64_t seq = 0;
    };
    // Record requests waiting for the residency gate, answered together.
    std::vector<HeldReply> held_records_;
    bool was_recording_ = false;
    int recording_take_ = 0;

    uint64_t next_housekeeping_ns_ = 0;
    bool fatal_said_ = false;
    int failed_take_ = 0;
    bool take_failed_shown_ = false;
};

}  // namespace sonora
