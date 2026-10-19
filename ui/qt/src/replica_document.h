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
    model::Id reserve_id() override;
    // Sends the session to the engine (ModelAction::Load) and shows it at
    // once.
    bool reset(model::Snapshot s, std::string& why) override;
    // ModelAction::FlushPluginState. `done` runs from the receive() that
    // brings the engine's reply, after that call has applied the updates the
    // engine sent before it (the engine replies after the last of them); a
    // lost link fails it. Refused at once while the link is down.
    void flush_plugin_state(model::Id insert, FlushDone done) override;

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
    // Lost track after having had the engine's document: a Resync is out,
    // the last good document is shown and edits are refused.
    bool resyncing() const { return replica_.history() != nullptr && !replica_.synced(); }
    // How many Resyncs were sent, for the tests.
    std::size_t resyncs_sent() const { return resyncs_sent_; }
    std::size_t in_flight() const { return pending_.size(); }
    // Why the engine refused a request; for the status line.
    std::function<void(const std::string&)> on_refused;
    // A File > Open was among the dropped requests: the window must stop
    // naming that file, or Save would write the engine's document over it.
    std::function<void()> on_load_abandoned;

private:
    struct Pending {
        enum class Kind { Apply, Undo, Redo, Load };
        Kind kind = Kind::Apply;
        uint64_t seq = 0;
        model::Operation op;                 // Apply
        std::optional<model::Snapshot> load; // Load
        bool answered = false;   // accepted; its update is still on its way
    };

    uint64_t send(api::ModelAction action, std::string bytes);
    void rebuild();

    Submit submit_;
    model::Snapshot placeholder_;   // shown, with the requests on top, until the first Snapshot
    model::Replica replica_;
    model::History shown_;
    std::deque<Pending> pending_;
    struct Flush {
        uint64_t seq = 0;
        FlushDone done;
    };
    std::vector<Flush> flushes_;   // awaiting the engine's reply
    model::Id next_unreserved_ = 0;
    bool offline_ = false;
    bool resync_pending_ = false;   // one Resync per loss, not one per failed update
    std::size_t resyncs_sent_ = 0;
};

}  // namespace sonora::ui
