// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/loop_take.h"

#include <algorithm>
#include <utility>

#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wrap_sidecar.h"

namespace sonora {

namespace {

constexpr uint64_t kLost = WrapSidecar::kLostWrap;

// The first wrap of a file whose first sample was recorded at
// `time_reference`: the sample at B, L samples late.
uint64_t first_wrap(uint64_t loop_to, uint64_t time_reference, uint32_t latency) {
    return time_reference <= loop_to ? loop_to - time_reference + latency : kLost;
}

}  // namespace

std::vector<uint64_t> complete_wraps(const std::vector<uint64_t>& known, std::size_t count,
                                     uint64_t frames, uint64_t loop_from, uint64_t loop_to,
                                     uint64_t time_reference, uint32_t latency, bool& derived) {
    derived = false;
    if (loop_to <= loop_from) return known;
    const uint64_t len = loop_to - loop_from;
    const bool until_end = count == kWrapsUntilEnd;
    std::vector<uint64_t> out = known;
    if (!until_end && count > out.size()) out.resize(count, kLost);
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (out[k] != kLost) continue;
        derived = true;
        if (k > 0 && out[k - 1] != kLost) {
            out[k] = out[k - 1] + len;
            continue;
        }
        std::size_t q = k + 1;
        while (q < known.size() && known[q] == kLost) ++q;
        if (q < known.size()) {
            const uint64_t back = static_cast<uint64_t>(q - k) * len;
            if (known[q] >= back) out[k] = known[q] - back;
            continue;
        }
        const uint64_t first = first_wrap(loop_to, time_reference, latency);
        if (first != kLost) out[k] = first + static_cast<uint64_t>(k) * len;
    }
    if (until_end) {
        uint64_t next = out.empty() ? first_wrap(loop_to, time_reference, latency)
                        : out.back() == kLost ? kLost
                                              : out.back() + len;
        while (next != kLost && next <= frames) {
            out.push_back(next);
            derived = true;
            next += len;
        }
    }
    return out;
}

std::vector<LoopPass> loop_passes(const std::vector<uint64_t>& wraps, uint64_t frames,
                                  uint64_t loop_from, uint64_t loop_to, uint64_t time_reference,
                                  uint32_t latency) {
    std::vector<LoopPass> out;
    if (loop_to <= loop_from) return out;
    const uint64_t len = loop_to - loop_from;
    // [offset, offset + length) of the file, playing `at` frames after A.
    const auto add = [&](std::size_t pass, uint64_t offset, uint64_t at, uint64_t want,
                         bool whole) {
        if (offset >= frames || want == 0) return;
        LoopPass p;
        p.pass = pass;
        p.offset = offset;
        p.at = at;
        p.length = std::min(want, frames - offset);
        p.complete = whole && p.length == len;
        out.push_back(p);
    };
    // Wrap k ends pass k + 1: a lost wrap keeps its place in the list.
    for (std::size_t k = 0; k < wraps.size(); ++k) {
        const uint64_t w = wraps[k];
        if (w == kLost) continue;
        // A track whose window opened late, inside this pass: the part it has.
        if (w < len) {
            add(k + 1, 0, len - w, w, false);
        } else {
            add(k + 1, w - len, 0, len, w <= frames);
        }
    }
    uint64_t tail = kLost;
    uint64_t at = 0;
    if (!wraps.empty()) {
        tail = wraps.back();
    } else {
        // Sample i was played at time_reference + i - L.
        const int64_t s = static_cast<int64_t>(loop_from) - static_cast<int64_t>(time_reference) +
                          static_cast<int64_t>(latency);
        tail = s >= 0 ? static_cast<uint64_t>(s) : 0;
        at = s >= 0 ? 0 : static_cast<uint64_t>(-s);
    }
    if (tail != kLost && at < len) add(wraps.size() + 1, tail, at, len - at, false);
    return out;
}

bool recover_loop_wraps(const std::string& take_path, RecoveredLoop& out) {
    WavReader file;
    std::string error;
    if (!file.open(take_path, error)) return false;
    RecoveredLoop r;
    if (!loop_of_description(file.description(), r.from, r.to)) return false;
    r.time_reference = file.time_reference();
    r.frames = static_cast<uint64_t>(file.frames());
    WrapList list;
    if (!read_wrap_sidecar(WrapSidecar::path_for(take_path), list, r.why)) {
        list.wraps.clear();
    } else if (list.loop_from != r.from || list.loop_to != r.to) {
        // The Description is the take's own; a list that disagrees with it
        // is not this take's.
        r.why = WrapSidecar::path_for(take_path) + ": its loop is not the take's";
        list.wraps.clear();
        list.latency = 0;
    }
    r.latency = list.latency;
    bool derived = false;
    r.wraps = complete_wraps(list.wraps, kWrapsUntilEnd, r.frames, r.from, r.to, r.time_reference,
                             r.latency, derived);
    r.by_length = derived;
    out = std::move(r);
    return true;
}

}  // namespace sonora
