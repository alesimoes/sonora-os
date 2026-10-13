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
    model::Id reserve_id() override;
    // Sends the session to the engine (ModelAction::Load) and shows it at
    // once.
    void reset(model::Snapshot s) override;

    // What the engine sent since the last call, in arrival order. Returns
    // true when what is shown changed.
    bool receive(const std::vector<api::ModelUpdate>& updates,
                 const std::vector<api::CommandReply>& replies);

    // The engine's document has arrived (its first Snapshot).
    bool synced() const { return replica_.synced(); }
    std::size_t in_flight() const { return pending_.size(); }
    // Why the engine refused a request; for the status line.
    std::function<void(const std::string&)> on_refused;

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
    model::Replica replica_;
    model::History shown_;
    std::deque<Pending> pending_;
    model::Id next_unreserved_ = 0;
};

}  // namespace sonora::ui
