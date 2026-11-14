#pragma once

// The editor's side of the PatternExchange (ADR-0169). publish() refuses while
// the audio thread still holds the spare slot; the editor then keeps its own
// copy and offers it again on its next tick. Nothing here waits, so a refused
// edit is late by one tick at most, and the newest edit is what goes out.

#include <cstdint>

#include "sonora/stepseq/pattern_exchange.h"

namespace sonora::stepseq {

class PatternPublisher {
public:
    explicit PatternPublisher(PatternExchange& exchange) noexcept : exchange_(exchange) {}

    // Editor thread, after every edit. False: refused, now pending.
    bool submit(const Pattern& editor_copy) noexcept {
        pending_ = !exchange_.publish(editor_copy);
        if (pending_) ++refusals_;
        return !pending_;
    }

    // Editor thread, on each tick while pending(). The current editor copy is
    // passed, not the refused one, so edits made in between are not lost.
    bool retry(const Pattern& editor_copy) noexcept {
        return !pending_ || submit(editor_copy);
    }

    bool pending() const noexcept { return pending_; }
    std::uint64_t refusals() const noexcept { return refusals_; }

private:
    PatternExchange& exchange_;
    bool pending_ = false;
    std::uint64_t refusals_ = 0;
};

} // namespace sonora::stepseq
