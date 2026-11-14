#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonora/loops/loop_entry.h"

namespace sonora::loops {

// Catalog backend (factory folder, user folder). Off the audio thread only.
class ILoopSource {
public:
    virtual ~ILoopSource() = default;
    virtual std::size_t count() const = 0;
    virtual const LoopEntry* at(std::size_t index) const = 0;
    virtual std::vector<LoopEntry> search(const LoopQuery& query) const = 0;
};

// Preview on the UI side; the engine's audio path is untouched.
class ILoopAudition {
public:
    virtual ~ILoopAudition() = default;
    // Returns false when the entry cannot be played.
    virtual bool start(const LoopEntry& entry, double session_bpm) = 0;
    virtual void stop() = 0;
    virtual bool playing() const = 0;
};

// Puts an entry on a track at a position through the normal edit operations.
class ILoopInserter {
public:
    virtual ~ILoopInserter() = default;
    // Returns false when refused (unknown track, unreadable file).
    virtual bool insert(const LoopEntry& entry, int track_index,
                        std::uint64_t position_frames) = 0;
};

}  // namespace sonora::loops
