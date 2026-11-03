// SPDX-License-Identifier: GPL-3.0-or-later
//
// The offset from a server's clock to steady_clock, as JackBackend keeps it
// (PR #77 RT audit L1). A bracket is (steady before, server, steady after);
// a wide one, stretched by the scheduler, moves the offset by up to half its
// width. Under sustained load every bracket after the first can be wide, so
// the first value must not be a wide one that then stays for good.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sonora/clock_offset.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::drivers::ClockOffset;

constexpr int64_t kTrue = 800'000'000;  // steady - server, as jackd2 measured

struct Bracket {
    int64_t t0, server, t1;
};

// A bracket around a server read taken at steady `at`, `width` wide, with
// the read `skew` after the bracket's middle.
Bracket around(int64_t at, int64_t width, int64_t skew = 0) {
    return {at - width / 2 - skew, at - kTrue, at + (width - width / 2) - skew};
}

// Hands out scripted brackets, then repeats the last.
struct Script {
    std::vector<Bracket> b;
    std::size_t next = 0;
    int calls = 0;
    Bracket operator()() {
        ++calls;
        const Bracket r = b[next < b.size() ? next : b.size() - 1];
        if (next < b.size()) ++next;
        return r;
    }
};

int test_a_narrow_first_bracket_is_trusted() {
    ClockOffset o;
    CHECK(!o.have());
    Script s{{around(1'000'000, 2'000)}};
    o.first(s);
    CHECK(o.have() && o.trusted());
    CHECK(o.offset() == kTrue);
    CHECK(s.calls == 1);
    return 0;
}

// L1: the first value is retried, bounded, until a bracket is narrow.
int test_the_first_value_is_retried_until_narrow() {
    ClockOffset o;
    Script s{{around(1'000'000, 900'000, 300'000), around(2'000'000, 400'000, 150'000),
              around(3'000'000, 5'000)}};
    o.first(s);
    CHECK(o.trusted());
    CHECK(s.calls == 3);
    CHECK(o.offset() == kTrue);
    return 0;
}

// Never more than kFirstTries brackets; then the narrowest is kept, and not
// trusted.
int test_the_retry_is_bounded() {
    ClockOffset o;
    Script s;
    for (int i = 0; i < 20; ++i) s.b.push_back(around(1'000'000 * (i + 1), 500'000 + 1'000 * i, 200'000));
    s.b[4] = around(5'000'000, 300'000, 100'000);  // the narrowest of them
    o.first(s);
    CHECK(s.calls == ClockOffset::kFirstTries);
    CHECK(o.have() && !o.trusted());
    CHECK(o.offset() == kTrue - 100'000);
    return 0;
}

// L1: a wide value that had to be taken does not persist. A narrower wide
// one improves it; the first narrow one replaces it and is trusted.
int test_a_wide_value_does_not_persist() {
    ClockOffset o;
    CHECK(o.offer(around(1'000'000, 2'000'000, 900'000)));
    CHECK(!o.trusted() && o.offset() == kTrue - 900'000);
    CHECK(!o.offer(around(2'000'000, 3'000'000, 1'200'000)));  // wider: no better
    CHECK(o.offset() == kTrue - 900'000);
    CHECK(o.offer(around(3'000'000, 600'000, 250'000)));
    CHECK(!o.trusted() && o.offset() == kTrue - 250'000);
    CHECK(o.offer(around(4'000'000, 3'000)));
    CHECK(o.trusted() && o.offset() == kTrue);
    return 0;
}

// Once trusted, a wide bracket is ignored, however it compares, and a
// narrow one follows the clocks' drift.
int test_trusted_ignores_wide_and_follows_narrow() {
    ClockOffset o;
    CHECK(o.offer(around(1'000'000, 3'000)));
    CHECK(!o.offer(around(2'000'000, ClockOffset::kMaxWidthNs + 1, 50'000)));
    CHECK(o.offset() == kTrue);
    Bracket drifted = around(3'000'000, 4'000);
    drifted.server -= 40;  // 40 ns of drift
    CHECK(o.offer(drifted));
    CHECK(o.offset() == kTrue + 40);
    return 0;
}

// A bracket that runs backwards is no bracket.
int test_a_reversed_bracket_is_refused() {
    ClockOffset o;
    CHECK(!o.offer(2'000'000, 0, 1'000'000));
    CHECK(!o.have());
    return 0;
}

// The offset is re-measured every 100 ms and can step down by tens of us (a
// bracket's half width) while the server's cycle time advances by one period,
// which can be shorter: callback_ns then went backwards (CI, RTSan job, "1
// decreases of callback_ns").
int test_a_downward_offset_step_never_lowers_the_stamp() {
    sonora::drivers::MonotonicStamp s;
    const int64_t cycle_ns = 5'000'000'000;
    const int64_t offset = 800'000'000;
    const uint64_t first = s.stamp(cycle_ns, offset);
    CHECK(first == static_cast<uint64_t>(cycle_ns + offset));
    // 1 us later on the server, 50 us lower offset: the raw sum is 49 us back.
    const uint64_t next = s.stamp(cycle_ns + 1'000, offset - 50'000);
    CHECK(next >= first);
    CHECK(s.stamp(cycle_ns + 2'000, offset - 50'000) >= next);
    // Once the server is past it, the stamp follows the clock again.
    CHECK(s.stamp(cycle_ns + 10'000'000, offset - 50'000) ==
          static_cast<uint64_t>(cycle_ns + 10'000'000 + offset - 50'000));
    // A cycle time that could not be read stays 0 and does not poison the clamp.
    CHECK(s.stamp(0, 0) == 0);
    CHECK(s.stamp(cycle_ns + 11'000'000, offset - 50'000) >
          static_cast<uint64_t>(cycle_ns + 10'000'000));
    s.reset();
    CHECK(s.stamp(1'000, 0) == 1'000);
    return 0;
}

}  // namespace

int main() {
    if (test_a_downward_offset_step_never_lowers_the_stamp()) return 1;
    if (test_a_narrow_first_bracket_is_trusted()) return 1;
    if (test_the_first_value_is_retried_until_narrow()) return 1;
    if (test_the_retry_is_bounded()) return 1;
    if (test_a_wide_value_does_not_persist()) return 1;
    if (test_trusted_ignores_wide_and_follows_narrow()) return 1;
    if (test_a_reversed_bracket_is_refused()) return 1;
    std::printf("clock_offset: ok\n");
    return 0;
}
