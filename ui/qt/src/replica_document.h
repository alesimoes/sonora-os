// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplicaDocument: the TimelineDocument on the engine's document (ADR-0009
// §2, ADR-0007 M7). The engine owns the session; this holds a
// model::Replica fed by the engine's updates, which is the truth, and shows
// that truth with the requests still in flight applied on top.
//
// Optimistic, so the Edit window answers a key in the same frame: submit(),
// undo(), redo() and reset() apply here at once and send the request. When
// the engine answers, the request leaves the in-flight list: accepted, its
// effect is now in the replica; refused, it is dropped and the reason is
// handed to `on_refused` for the status line. Either way what is shown is
// rebuilt as the replica plus what is still in flight, so it never drifts
// from the engine for longer than a round trip.
//
// Lost track (an update that does not follow on, Replica::receive): one
// Resync is sent, however many updates fail before the Snapshot it asks for
// arrives. Until then the last document this replica held is still shown,
// never the placeholder: an empty timeline would say the session is gone.
// Edits are refused meanwhile, since what is shown may be behind the
// engine, and an edit resolved against it could mean something else there.
// If the engine refuses the Resync, its reason goes to `on_refused`, and the
// next edit asks again (and is refused with that reason).
//
// An accepted request leaves the in-flight list once an update has been
// applied after its reply (the engine replies just before the update). In
// the tick that loses track, its update may be the one that applied or one
// that failed; it is dropped either way. Kept, an applied Undo would be
// shown twice; dropped, an edit whose update failed is missing for the one
// round trip until the Snapshot, which shows what the engine holds.
//
// A lost connection (set_link): a request the engine has not answered went
// over a connection that no longer exists, or was dropped unsent, so its
// fate is unknown and it is dropped, never replayed on the Snapshot the next
// connection brings. That Snapshot is the engine's document, which is what
// is shown; one notice says how many edits were dropped. An accepted request
// whose update was lost stays until that Snapshot, which contains it. While
// the link is down, edits and File > Open are refused outright.
//
// Ids: an operation carries the ids it introduces (reserve_id()). Two
// interfaces may pick the same one; the engine refuses the second, and that
// interface sees the refusal. Single-operator use never meets it.
//
// No Qt here, so it is tested without a display.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "sonora/api/messages.h"
#include "sonora/model/history.h"
#include "sonora/model/replication.h"
#include "timeline_document.h"

namespace sonora::ui {

class ReplicaDocument final : public TimelineDocument {
public:
    // Sends one request to the engine and returns its seq.
    using Submit = std::function<uint64_t(const api::ModelRequest&)>;

    // `placeholder` is shown until the engine's first Snapshot arrives.
    ReplicaDocument(Submit submit, model::Snapshot placeholder);

    const model::Snapshot& snapshot() const override { return shown_.current(); }
    bool submit(const model::Operation& op, std::string& why) override;
    bool undo(std::string& why) override;
    bool redo(std::string& why) override;
    bool can_undo() const override { return shown_.can_undo(); }
    bool can_redo() const override { return shown_.can_redo(); }
    std::string undo_label() const override { return shown_.undo_label(); }
    std::string redo_label() const override { return shown_.redo_label(); }
    // What is shown: the engine's steps with the requests in flight on top.
    const model::History& history() const override { return shown_; }
    model::Id reserve_id() override;
    // Sends the session to the engine and shows it at once: its MIDI chunk
    // table as ModelAction::LoadPart requests of at most
    // api::wire::kMaxPartBytes, then ModelAction::Load with their index and
    // the session file (ADR-0015 §4). Refused by name instead, with nothing
    // shown or sent, when the parts together are over max_snapshot_bytes(),
    // or one chunk alone is over a part. The Load names `file` (protocol 16).
    using TimelineDocument::reset;
    bool reset(model::Snapshot s, const std::string& file, std::string& why) override;
    // This interface's cap on a Snapshot's chunk table and a Load's
    // sequence files (ADR-0015 §4: the replica's own, an interface setting).
    void set_max_snapshot_bytes(uint64_t bytes) { replica_.set_max_snapshot_bytes(bytes); }
    uint64_t max_snapshot_bytes() const { return replica_.max_snapshot_bytes(); }
    // ModelAction::FlushPluginState. `done` runs from the receive() that
    // brings the engine's reply, after that call has applied the updates the
    // engine sent before it (the engine replies after the last of them); a
    // lost link fails it. Refused at once while the link is down.
    void flush_plugin_state(model::Id insert, FlushDone done) override;
    // ModelAction::Save. `done` runs from the receive() that brings the
    // engine's reply; a lost link fails it. Refused at once while the link
    // is down.
    void save_session(const std::string& file, std::string bytes, SaveDone done) override;
    // ModelAction::SaveAs, the same way.
    void save_session_as(const std::string& file, uint32_t flags, std::string bytes,
                         SaveDone done) override;

    // What the engine sent since the last call, in arrival order. Returns
    // true when what is shown changed.
    bool receive(const std::vector<api::ModelUpdate>& updates,
                 const std::vector<api::CommandReply>& replies);
    // The connection to the engine went (`up` false) or was replaced by a new
    // one (`up` true); called after receive() has had what the old one
    // brought. Returns true when what is shown changed.
    bool set_link(bool up);

    // The engine's document has arrived (its first Snapshot).
    bool synced() const { return replica_.synced(); }
    // A Snapshot in parts is arriving: what is shown is the document before
    // it, and edits are refused until its window has decoded.
    bool attaching() const { return replica_.attaching(); }
    // Lost track after having had the engine's document: a Resync is out,
    // the last good document is shown and edits are refused.
    bool resyncing() const { return replica_.history() != nullptr && !replica_.synced(); }
    // How many Resyncs were sent, for the tests.
    std::size_t resyncs_sent() const { return resyncs_sent_; }
    std::size_t in_flight() const { return pending_.size(); }
    // Why the engine refused a request; for the status line.
    std::function<void(const std::string&)> on_refused;
    // A File > Open did not happen, and what is shown is no longer its
    // session: the window must stop naming that file, or Save would write
    // another document over it. `refused` is empty when the request was
    // dropped with the link (what the engine holds is unknown until its
    // Snapshot), and is the engine's reason when the engine refused it (the
    // engine kept the session it had). `later_load`: another Open is still
    // in flight, so what is shown is that one's session. Called once the
    // replica is rebuilt; while unset, a refusal goes to `on_refused`.
    std::function<void(const std::string& refused, bool later_load)> on_load_abandoned;

private:
    struct Pending {
        enum class Kind { Apply, Undo, Redo, Load, LoadPart };
        Kind kind = Kind::Apply;
        uint64_t seq = 0;
        model::Operation op;                 // Apply
        std::optional<model::Snapshot> load; // Load
        bool answered = false;   // accepted; its update is still on its way
    };

    uint64_t send(api::ModelAction action, std::string bytes);
    void request_resync();
    // True, with `why`, when an edit must be refused now: offline, a Load
    // the engine has not answered, or behind the engine. A Resync the engine refused is asked for again here, on the
    // operator's next edit, so a refusal never leaves the window waiting on
    // nothing, and a refusing engine is not asked in a loop.
    bool refuse_while_lost(std::string& why);
    void rebuild();

    Submit submit_;
    model::Snapshot placeholder_;   // shown, with the requests on top, until the first Snapshot
    model::Replica replica_;
    model::History shown_;
    std::deque<Pending> pending_;
    // A FlushPluginState or a Save, awaiting the engine's reply: what to say
    // when the engine refuses without a reason, and when the link goes.
    struct Flush {
        uint64_t seq = 0;
        FlushDone done;
        const char* refused = "";
        const char* lost = "";
    };
    std::vector<Flush> flushes_;
    model::Id next_unreserved_ = 0;
    bool offline_ = false;
    bool resync_pending_ = false;   // one Resync per loss, not one per failed update
    // A Snapshot in parts was abandoned at a bad part; its window is still
    // on its way and is not the one the Resync brings.
    bool abandoned_ = false;
    uint64_t resync_seq_ = 0;       // the Resync out, so its refusal is seen
    // Why the engine refused the last Resync; while it is set and the
    // replica is still lost, the next edit asks again (refuse_while_lost).
    std::string resync_refused_;
    std::size_t resyncs_sent_ = 0;
};

}  // namespace sonora::ui
