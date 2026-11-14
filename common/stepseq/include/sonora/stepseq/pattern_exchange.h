#pragma once

// Hands an immutable Pattern snapshot from the UI thread to the audio thread
// without locks (ADR-0169). Two preallocated slots: the UI copies its edited
// pattern into the slot nobody reads and publishes it; the audio thread takes
// the published one at the start of each block and says which one it holds.

#include <atomic>

#include "sonora/stepseq/pattern.h"

namespace sonora::stepseq {

class PatternExchange {
public:
    PatternExchange() noexcept {
        published_.store(&slots_[0]);
        in_use_.store(&slots_[0]);
    }
    PatternExchange(const PatternExchange&) = delete;
    PatternExchange& operator=(const PatternExchange&) = delete;

    // UI thread only. Returns false, without waiting, while the audio thread
    // still holds the spare slot; the editor retries on its next tick. The
    // spare slot is never the published one, and the audio thread only moves
    // to the published one, so once in_use_ is seen off the spare it stays off.
    bool publish(const Pattern& edited) noexcept {
        Pattern* spare = published_.load() == &slots_[0] ? &slots_[1] : &slots_[0];
        if (in_use_.load() == spare) return false;
        *spare = edited;
        published_.store(spare);
        return true;
    }

    // Audio thread only, once per block; the reference stays valid until the
    // next acquire() on this thread.
    const Pattern& acquire() noexcept {
        Pattern* p = published_.load();
        in_use_.store(p);
        return *p;
    }

private:
    Pattern slots_[2];
    // seq_cst on both: publish()'s check of in_use_ must order against
    // acquire()'s store, which acquire/release alone does not guarantee.
    std::atomic<Pattern*> published_{nullptr};
    std::atomic<Pattern*> in_use_{nullptr};
};

} // namespace sonora::stepseq
