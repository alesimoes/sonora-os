// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loop take's passes, in its file's frames (ADR-0016 §1, §5): one
// arithmetic for a take engine/control places at stop, a take whose wraps a
// full boundary ring lost, and a take recovery finds after a crash. Every
// pass is placed; nothing recorded is left off the timeline.
//
// Not for the audio thread: it allocates, and recover_loop_wraps() reads
// files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonora {

// complete_wraps() with no count: as many wraps as the file holds.
constexpr std::size_t kWrapsUntilEnd = ~std::size_t{0};

// `known` with each lost wrap (WrapSidecar::kLostWrap) derived, and
// extended to `count` wraps, or while they fall inside `frames` for
// kWrapsUntilEnd. A derived wrap is the nearest known one before it plus
// B - A per pass, else the nearest after it less that, else the first wrap
// the loop gives a file starting at `time_reference`: B - time_reference +
// latency. One the arithmetic puts before the file stays lost. `derived`
// says whether any was. Exact only if no capture overrun shifted the
// samples after it (session.cpp drops samples on a short capture-ring
// write). A late file's leading WrapSidecar::kBeforeFile wraps are the
// take's before it opened: first wrap counts from after them, and one of
// them stays kBeforeFile unless the input latency put it inside the file,
// which is not derived.
std::vector<uint64_t> complete_wraps(const std::vector<uint64_t>& known, std::size_t count,
                                     uint64_t frames, uint64_t loop_from, uint64_t loop_to,
                                     uint64_t time_reference, uint32_t latency, bool& derived);

// One pass: the file's frames [offset, offset + length), which play from
// `at` frames after A: 0 unless the track's window opened inside the pass.
// Complete when it is B - A long and all of it reached the file. `pass`
// is which of the take's passes it is, from 1, the same on every track: a
// track whose window opened late, or whose wrap was lost, skips numbers
// rather than renumbering the passes it has.
struct LoopPass {
    uint64_t offset = 0;
    uint64_t at = 0;
    uint64_t length = 0;
    bool complete = false;
    std::size_t pass = 0;
};

// Pass k ends at wrap k; the pass after the last wrap, if the file goes on
// past it, is cut short by the stop. A take stopped before its first wrap
// has that one pass, from A; one stopped in its pre-roll has none. A pass
// whose wrap is lost, or before the file, is left out: complete_wraps()
// first.
std::vector<LoopPass> loop_passes(const std::vector<uint64_t>& wraps, uint64_t frames,
                                  uint64_t loop_from, uint64_t loop_to, uint64_t time_reference,
                                  uint32_t latency);

// What recovery knows of a loop take's file.
struct RecoveredLoop {
    uint64_t from = 0;
    uint64_t to = 0;
    uint64_t time_reference = 0;
    uint64_t frames = 0;
    // The wrap list's L; 0 without a list, the only L recorded so far.
    uint32_t latency = 0;
    std::vector<uint64_t> wraps;   // complete_wraps(), to the file's end
    // Some wraps were derived: the wrap list was missing, stopped early, or
    // held lost wraps. The offer says the passes were placed by loop length.
    bool by_length = false;
    std::string why;               // the wrap list's problem, when there was one
};

// The take at `take_path` as a loop take: A and B from its Description's
// loop=, the wraps and L from `<take_path>.wraps`, the rest derived. False
// if the file does not read or is not a loop take's.
bool recover_loop_wraps(const std::string& take_path, RecoveredLoop& out);

}  // namespace sonora
