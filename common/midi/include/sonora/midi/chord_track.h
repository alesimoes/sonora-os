// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Chords placed on the timeline, sorted by tick, at most one per tick.
// Control thread only (ADR-0126).

#include <cstddef>
#include <optional>
#include <vector>

#include "sonora/midi/chord.h"
#include "sonora/midi/event.h"

namespace sonora::midi {

struct ChordChange {
    Tick tick = 0;
    Chord chord;
};

class ChordTrack {
public:
    // Replaces the chord already at `tick`.
    void set(Tick tick, const Chord& chord);
    // True when a chord was at `tick`.
    bool remove(Tick tick);
    void clear() { changes_.clear(); }

    // The chord sounding at `tick`: the last change at or before it.
    std::optional<Chord> chord_at(Tick tick) const;

    const std::vector<ChordChange>& changes() const { return changes_; }
    std::size_t size() const { return changes_.size(); }
    bool empty() const { return changes_.empty(); }

private:
    std::vector<ChordChange> changes_;
};

}  // namespace sonora::midi
