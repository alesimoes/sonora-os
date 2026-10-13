// SPDX-License-Identifier: GPL-3.0-or-later
//
// The document the engine keeps until the document thread and the session
// file are wired in: one model track per engine track, and every closed take
// placed where it was recorded, so what was recorded can be played back.
// Lengths and positions come from the files on disk (their data size and
// BWF TimeReference), never from the transport (ADR-0009, Consequences:
// "the length in the session is the length on disk").
//
// Control thread only. It allocates and reads files.
#pragma once

#include <string>

#include "sonora/model/history.h"

namespace sonora {

class Session;

class TakeDocument {
public:
    explicit TakeDocument(const Session& session);

    // Places `relative_path` (from the session folder) on engine track
    // `track` at the file's TimeReference, overwriting what is beneath it.
    bool place_file(const std::string& relative_path, std::size_t track, std::string& error);
    // Every track's file of the take that just closed, one undo step each.
    // Tracks that wrote nothing are skipped.
    bool add_closed_take(const Session& session, std::string& error);

    const model::Snapshot& current() const { return history_.current(); }
    model::History& history() { return history_; }

private:
    std::string dir_;
    model::History history_;
    std::vector<model::Id> playlists_;   // per engine track
};

}  // namespace sonora
