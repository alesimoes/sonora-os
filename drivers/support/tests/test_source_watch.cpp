// SPDX-License-Identifier: GPL-3.0-or-later
//
// DeviceGone ends a take, so it must mean what it says: a physical capture
// port that feeds one of our inputs went away. A port the operator unpatched
// earlier, and a port that only reused a forgotten one's id, are not that.
//
// The order of events below is the one measured under pw-jack 1.4.2 when a
// physical source's node is destroyed: the link's disconnect first, then the
// port's unregistration, delivered in the same batch.
#include <cstdint>
#include <cstdio>
#include <string>

#include "sonora/source_watch.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::drivers::SourceWatch;

constexpr uint64_t kSecond = 1000000000ull;
const std::string kUmc = "UMC1820 Pro:capture_AUX0";

int test_linked_source_removed_is_gone() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    CHECK(w.removed(40, kUmc.c_str(), 10 * kSecond, gone));
    CHECK(gone == kUmc);
    return 0;
}

// pw-jack reports the link going before the port does. Forgetting on the
// disconnect alone would miss every unplug.
int test_unplug_disconnects_first() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    w.unlinked(40, kUmc, 5 * kSecond);
    CHECK(w.removed(40, kUmc.c_str(), 5 * kSecond, gone));
    CHECK(gone == kUmc);
    return 0;
}

// Unpatched by hand and gone later: the take never depended on it.
int test_unpatched_source_is_forgotten() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    w.unlinked(40, kUmc, 5 * kSecond);
    CHECK(!w.removed(40, kUmc.c_str(), 5 * kSecond + SourceWatch::kUnlinkGraceNs,
                     gone));
    CHECK(gone.empty());
    return 0;
}

int test_unknown_port_is_not_gone() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    CHECK(!w.removed(41, "Built-in Audio Pro:capture_AUX0", kSecond, gone));
    CHECK(gone.empty());
    return 0;
}

// jackd2 reuses port ids. A different port under a remembered id is not
// the remembered port.
int test_reused_id_is_matched_by_name() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    CHECK(!w.removed(40, "pw-play:output_FL", kSecond, gone));
    CHECK(gone.empty());
    // The stale entry is gone with the mismatch: the real port's removal,
    // should it still arrive under its id, finds nothing to end.
    CHECK(!w.removed(40, kUmc.c_str(), kSecond, gone));
    return 0;
}

// A new link under a remembered id with another name replaces the entry.
int test_relinked_id_takes_the_new_name() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    w.linked(40, "UMC1820 Pro:capture_AUX1", kSecond);
    CHECK(w.removed(40, "UMC1820 Pro:capture_AUX1", 2 * kSecond, gone));
    CHECK(gone == "UMC1820 Pro:capture_AUX1");
    return 0;
}

// One source patched to two inputs and unpatched from one still feeds the
// other.
int test_source_with_two_links_stays_until_both_go() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    w.linked(40, kUmc, 0);
    w.unlinked(40, kUmc, kSecond);
    CHECK(w.removed(40, kUmc.c_str(), 10 * kSecond, gone));
    return 0;
}

// No name to compare: the id is all there is.
int test_unnamed_removal_matches_by_id() {
    SourceWatch w;
    std::string gone;
    w.linked(40, kUmc, 0);
    CHECK(w.removed(40, nullptr, kSecond, gone));
    CHECK(gone == kUmc);
    return 0;
}

// Long-forgotten entries do not pile up over a day of patching.
int test_expired_entries_are_dropped() {
    SourceWatch w;
    for (uint32_t i = 0; i < 100; ++i) {
        w.linked(i, "src:" + std::to_string(i), i * 10 * kSecond);
        w.unlinked(i, "src:" + std::to_string(i), i * 10 * kSecond);
    }
    CHECK(w.size() <= 1);
    return 0;
}

}  // namespace

int main() {
    if (test_linked_source_removed_is_gone() != 0) return 1;
    if (test_unplug_disconnects_first() != 0) return 1;
    if (test_unpatched_source_is_forgotten() != 0) return 1;
    if (test_unknown_port_is_not_gone() != 0) return 1;
    if (test_reused_id_is_matched_by_name() != 0) return 1;
    if (test_relinked_id_takes_the_new_name() != 0) return 1;
    if (test_source_with_two_links_stays_until_both_go() != 0) return 1;
    if (test_unnamed_removal_matches_by_id() != 0) return 1;
    if (test_expired_entries_are_dropped() != 0) return 1;
    std::printf("test_source_watch: ok\n");
    return 0;
}
