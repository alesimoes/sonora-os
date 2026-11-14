// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/chord_track.h"

#include <algorithm>
#include <iterator>

namespace sonora::midi {

namespace {
auto lower(std::vector<ChordChange>& v, Tick tick) {
    return std::lower_bound(v.begin(), v.end(), tick,
                            [](const ChordChange& c, Tick t) { return c.tick < t; });
}
}  // namespace

void ChordTrack::set(Tick tick, const Chord& chord) {
    auto it = lower(changes_, tick);
    if (it != changes_.end() && it->tick == tick)
        it->chord = chord;
    else
        changes_.insert(it, ChordChange{tick, chord});
}

bool ChordTrack::remove(Tick tick) {
    auto it = lower(changes_, tick);
    if (it == changes_.end() || it->tick != tick) return false;
    changes_.erase(it);
    return true;
}

std::optional<Chord> ChordTrack::chord_at(Tick tick) const {
    auto it = std::upper_bound(
        changes_.begin(), changes_.end(), tick,
        [](Tick t, const ChordChange& c) { return t < c.tick; });
    if (it == changes_.begin()) return std::nullopt;
    return std::prev(it)->chord;
}

}  // namespace sonora::midi
