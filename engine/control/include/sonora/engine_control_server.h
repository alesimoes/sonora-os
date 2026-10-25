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
// Snapshots (ADR-0015 §4) are encoded off the document thread: the control
// thread copies the history, which shares every state, and a worker encodes
// the copy into a header, the chunk table's parts and the window. Whatever
// else is sent to that client meanwhile waits behind it, in order, so a
// replica sees nothing between its Snapshot's header and window. The parts
// go out as the client's transport drains them (Queued), at most two parts
// ahead, so a slow interface delays its own attach and nobody else's. One
// encoded Snapshot is held until every client it is for has been sent it;
// clients asking at one revision share it, and a client asking again while
// one it has not begun still waits is answered by that one, since what
// waits behind it brings the replica to now. What waits behind a Snapshot
// is bounded per client (Options::max_held_bytes): past it the client is
// not reading and is refused, as the socket drops one it cannot queue for.
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
// File > Save (ModelAction::Save, ADR-0015 update of 2026-10-25) is the
// saver thread's (session_saver.h): the control thread hands it the bytes
// and the document as it is, and answers the client when the save is done,
// in order with whatever else that client is sent. While a take is open
// every client is told "Saving after the next checkpoint".
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
#include "sonora/model/operations.h"
#include "sonora/orphan_offer.h"
#include "sonora/plugin_hosting.h"
#include "sonora/session_commands.h"
#include "sonora/state_publisher.h"

namespace sonora {

class PlaybackController;
class PluginHosting;
class PluginScanner;
class SessionSaver;
class Session;
namespace backend {
class AudioBackend;
}
namespace model {
class Document;
class LoadParts;
struct ReplicationUpdate;
struct Snapshot;
}

class EngineControlServer {
public:
    // One frame (api/wire.h), immutable and shared: the catalog's frames
    // are split once by the scan worker and handed to every client as they
    // are, never copied per client.
    using Frame = std::shared_ptr<const std::string>;
    using Outbound = std::function<void(Frame frame)>;
    // Called on the control thread when a client is dropped for what it
    // sent; the transport then closes the connection. `why` names the fault.
    using Refuse = std::function<void(const std::string& why)>;
    // Bytes the client's transport has taken and not yet written, asked on
    // the control thread; empty for a transport with no limit (in process).
    using Queued = std::function<std::size_t()>;
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
        // Plugin hosting (ADR-0012 checklist 5). Empty: none, and every
        // insert shows faulted (§5.5). Otherwise the server builds
        // PluginHosting once the backend's period is known (item 4 of the
        // amendment of 2026-10-14), or at once with `plugin_block_frames`
        // when there is no backend (tests).
        std::string plugin_host;
        std::vector<std::string> plugin_bundles;
        uint32_t plugin_block_frames = 0;
        // The plugin catalog's cache file (ADR-0012, amendment of
        // 2026-10-19, item 4 and change A: resolved by the entry layer).
        // Empty: the catalog lives in memory for the run.
        std::string plugin_cache;
        // The image build id, in the cache header (change B).
        std::string plugin_build_id;
        // Q1: a scan at every engine start. Off only in tests of the chains.
        bool plugin_scan_at_start = true;
        // Probe slots, one of them reserved for chain starts (change E); 0
        // is half the hardware threads, at least 2.
        uint32_t plugin_probe_slots = 0;
        // How long play from a freshly opened session waits for its
        // plug-ins (the owner's decision of 2026-10-18; §5.3's Ready bound).
        // Record never waits.
        uint64_t plugin_open_wait_ms = 10000;
        // MIDI's budget (ADR-0015 §5, owner decision O3): notes, events and
        // SysEx messages in the document, undo history's own included. Over
        // it undo history goes first; a session over it is refused at open,
        // and an edit still over it once history is gone is refused. A
        // Notice warns from 90 %. An engine setting, not a session key, so
        // a session over it is opened by raising this. 2^24, as
        // model::History::kDefaultMidiMaxEntries, which this header does
        // not see.
        uint64_t midi_max_entries = uint64_t{1} << 24;
        // What may wait for one client behind its Snapshot, the Snapshot
        // itself apart: ControlSocketServer's max_queued_bytes, which the
        // pacing would otherwise get round.
        std::size_t max_held_bytes = api::wire::kMaxMessageBytes + (std::size_t{16} << 20);
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
    ClientId attach(Outbound out, Refuse refuse, Queued queued = {});
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
    // Snapshots encoded on the worker since construction, and the longest
    // the control thread was held taking one's copy, in microseconds
    // (ADR-0015 Unverified 7).
    uint64_t snapshots_encoded() const { return snapshots_encoded_.load(std::memory_order_acquire); }
    // Snapshots asked for, one per client an attach, Resync or Load is for:
    // each has been answered, shared or queued by the time it is counted.
    uint64_t snapshots_asked() const { return snapshots_asked_.load(std::memory_order_acquire); }
    uint64_t longest_snapshot_copy_us() const {
        return longest_copy_us_.load(std::memory_order_acquire);
    }

    // --- the control thread, housekeeping, or before start() ---
    void post_event(const api::EngineEvent& event);
    // An event for the clients attached now only: not kept, so a client
    // that attaches later is not sent it.
    void post_transient(const api::EngineEvent& event);
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
    struct SnapshotJob;
    // What waits to go to a client behind a Snapshot: the Snapshot itself,
    // a message, or a reply whose result is the Snapshot's (a Resync's).
    struct Held {
        std::shared_ptr<SnapshotJob> job;
        std::string bytes;
        std::vector<Frame> frames;
        uint64_t reply_to = 0;
    };
    struct Client {
        Outbound out;
        Refuse refuse;
        Queued queued;
        api::wire::Reassembler reassembler;
        // Frames held back behind a large message that has no slot yet.
        std::deque<std::string> parked;
        std::deque<Held> held;
        std::size_t job_sent = 0;   // messages of the first held job already out
        std::size_t held_bytes = 0;   // of the messages in `held`
        bool overflowed = false;
    };
    struct Inbound {
        enum class Kind { Attach, Frame, Detach };
        Kind kind = Kind::Frame;
        ClientId client = 0;
        std::string bytes;
        Outbound out;
        Refuse refuse;
        Queued queued;
    };
    // A Load in parts from one client (protocol 10).
    struct LoadInParts {
        std::unique_ptr<model::LoadParts> parts;
        std::string refused;
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
    // The client is gone: its backlog, and any ride it left unreleased.
    void forget(ClientId id);
    // Refuses each client that went past max_held_bytes.
    void drop_overflowed();
    // The document's mix onto the engine's tracks; every ride ends there.
    void apply_mix();
    // Handled frames' bytes leave their clients' backlogs.
    void release_handled();
    void greet(ClientId id);
    // `bytes` or `frames` to the client now, past anything held.
    void write_to(Client& client, const std::string& bytes);
    void write_frames(Client& client, const std::vector<Frame>& frames);
    void refuse(ClientId id, const std::string& why);
    void handle(ClientId from, const std::string& bytes);
    void tick();
    // To every client.
    void send(const std::string& bytes);
    void send_to(Client& client, const std::string& bytes);
    void send_to(ClientId id, const std::string& bytes);
    // Frames already split, shared as they are.
    void send_frames(Client& client, const std::vector<Frame>& frames);
    // A Snapshot of the document as it is now, for each of `to`, behind
    // whatever each is still waiting for. With `reply_seq`, the asker's
    // reply follows it, Ok or the reason it could not be encoded.
    void send_snapshot(const std::vector<ClientId>& to, uint64_t reply_seq = 0);
    // Each client's held messages out, as far as finished Snapshots and the
    // transports' room allow. True while a client has a Snapshot part-sent.
    bool pump_snapshots();
    // The Snapshot to send now: the last one made, while a client still
    // waits for it and the document is where it was taken, else a new one.
    std::shared_ptr<SnapshotJob> snapshot_job();
    void encode_loop();
    void encode(SnapshotJob& job);
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
    // A Notice once the document's MIDI reaches 90 % of midi_max_entries
    // (ADR-0015 §5), so the operator learns of the budget before a take, not
    // after. Said again only after it has fallen below and risen again.
    void warn_midi_budget();
    bool midi_warned_ = false;
    // A FlushPluginState (ADR-0012 §8): which inserts, and whether they can
    // be flushed. False, with `why`, to be answered Failed at once. With
    // hosting the reply is held: the running plugins' states are read
    // (PluginHosting::request_states), written by the saver thread, then
    // named in the document, and only then answered (finish_flush).
    bool flush_plugin_state(ClientId from, const api::ModelRequest& m, std::string& why);
    struct PendingFlush {
        ClientId to = 0;
        uint64_t seq = 0;
        // session_generation_ when asked: a session opened meanwhile has
        // other inserts, possibly under the same ids, and gets nothing.
        uint64_t generation = 0;
        // hosting_'s flush id while its states are read; 0 once they are
        // in `flush`, at the saver.
        uint64_t reading = 0;
        PluginHosting::Flush flush;
    };
    // By the server's own id, the saver job's: hosting_'s ids start again
    // with each activation.
    std::map<uint64_t, PendingFlush> flushes_;
    uint64_t next_flush_ = 1;
    // Hosting's finished reads, to the saver (or answered when there is
    // nothing to write).
    void follow_flushes();
    // Reads hosting_ was asked for, forgotten when it goes.
    void drop_reading_flushes();
    // The saver wrote a flush's states (`written`), or could not (`why`).
    void finish_flush(uint64_t id, bool written, const std::string& why);
    // Every take the disk thread has closed joins the document: at a stop,
    // a punch-out, or a take started over another.
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
    // The plugin scan worker (ADR-0012, amendment of 2026-10-19, item 3):
    // for the life of the server, which is the engine process's, so a
    // reopen at another period does not rescan. Null when no host is
    // configured. Destroyed after hosting_, whose probes it answers.
    std::unique_ptr<PluginScanner> scanner_;
    // The catalog generation the clients were last sent.
    uint64_t catalog_sent_ = 0;
    // The catalog to clients, the scanner's notices said, once per turn.
    void follow_scanner();
    // The catalog a client is sent after the Snapshot.
    void send_catalog(Client& client);
    // Item 6: AddInsert and ReplaceInsert against the catalog. Empty when
    // the plugin may be inserted; the refusal otherwise.
    std::string catalog_refusal(const model::Snapshot& doc, model::MemberRef member) const;
    // A SetSettings that moves the loop range while a loop take records
    // (ADR-0016 §1), by name; empty otherwise.
    std::string loop_refusal(const model::Snapshot& doc, model::MemberRef member) const;
    // The plugin side, for one backend activation; null until the period is
    // known, and when no host is configured. Released in after_deactivate().
    std::unique_ptr<PluginHosting> hosting_;
    // Builds hosting_ when it can, once (the control thread).
    void start_hosting();
    // Its document operations (editor gestures) applied and sent.
    void apply_hosting_ops(std::vector<model::Operation>& ops);
    bool hosting_tried_ = false;
    // Play from a freshly opened session, held until its plug-ins have
    // loaded or failed, or until open_deadline_ms_ (the owner's decision of
    // 2026-10-18). Record is never held.
    struct HeldPlay {
        ClientId to = 0;
        uint64_t seq = 0;
        std::string command;
        CommandContext ctx;
    };
    std::vector<HeldPlay> held_plays_;
    uint64_t open_deadline_ms_ = 0;
    bool plugins_loading() const;
    // Held plays dispatched (`cancelled`: answered Failed instead).
    void release_plays(bool cancelled);
    // The plans published when the state block last took the playing
    // plan's compensation.
    uint64_t compensation_shown_for_ = ~uint64_t{0};
    uint32_t silent_midi_said_ = 0;   // the MIDI tracks the last notice counted

    CommandRegistry registry_;
    std::vector<std::string> implemented_;
    // The take install_session_commands numbers. The control thread's own;
    // the publisher sees it through set_take().
    int take_ = 1;
    bool quit_ = false;   // app.quit's flag; never set, as the engine never runs it

    // The control thread's, or start()'s before that thread exists.
    std::map<ClientId, Client> clients_;
    // By engine track, the client whose ParameterChange last set its gain
    // since the document's faders were last applied: a rider that leaves
    // has that track's gain put back, and no other client's ride moves.
    std::map<uint32_t, ClientId> riders_;
    // Events that still stand, encoded, for a client that attaches later:
    // what a restarted interface must find on screen.
    std::vector<std::string> notices_;
    // One per insert with a fault standing, by insert id: kept apart from
    // notices_ so a cleared fault is never replayed and faults never crowd
    // a device notice out of kMaxStandingNotices.
    std::map<uint64_t, std::string> plugin_faults_;
    // The text of each standing fault, to say a change only once.
    std::map<uint64_t, std::string> fault_text_;
    std::string inputs_missing_;
    std::string take_failed_;
    std::string fatal_;
    // The takes on disk the open session does not name (orphan_offer.h),
    // offered as one standing RecoveryOffer until placed or left. Found at
    // every session open; while a take records, once it has closed, so the
    // take being written is never one of them.
    OrphanOffer orphans_;
    std::string recovery_offer_;
    bool orphan_scan_pending_ = false;
    void offer_orphans();
    // The offer taken back, with an empty RecoveryOffer, if one stands.
    void withdraw_orphans();

    // The control thread's: clients reassembling a large message, clients
    // waiting for a slot (in arrival order), and bytes handled since the
    // backlogs were last brought down.
    std::set<ClientId> large_;
    std::deque<ClientId> parked_;
    std::map<ClientId, std::size_t> handled_;
    std::map<ClientId, LoadInParts> loads_;
    // Bumped by every Load, so a Snapshot encoded from the session before
    // never moves the new one's undo floor.
    uint64_t session_generation_ = 0;
    bool sending_snapshot_ = false;

    // The Snapshot worker's queue; its own lock, never held with mutex_.
    std::mutex jobs_mutex_;
    std::condition_variable jobs_wake_;
    std::deque<std::shared_ptr<SnapshotJob>> jobs_;
    bool jobs_stop_ = false;
    std::thread encoder_;
    std::weak_ptr<SnapshotJob> last_job_;
    bool overflowed_ = false;
    // Set by the worker when a Snapshot is ready; under mutex_, so the
    // control thread's wait sees it.
    bool snapshot_ready_ = false;
    // File > Save (ModelAction::Save) on the saver thread, which sets
    // save_ready_ under mutex_ when a result waits; answered in tick().
    std::unique_ptr<SessionSaver> saver_;
    bool save_ready_ = false;
    // A Save, handed to the saver; false when it was refused at once.
    bool save(ClientId from, const api::ModelRequest& m, std::string& why);
    // Results to their clients, and the saver told what the transport does.
    void follow_saver();
    std::atomic<uint64_t> snapshots_encoded_{0};
    std::atomic<uint64_t> snapshots_asked_{0};
    std::atomic<uint64_t> longest_copy_us_{0};

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

    uint64_t next_housekeeping_ns_ = 0;
    bool fatal_said_ = false;
    int failed_take_ = 0;
    bool take_failed_shown_ = false;
};

}  // namespace sonora
