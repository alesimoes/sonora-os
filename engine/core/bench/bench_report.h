// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-graph-bench's verdict (ADR-0218 §2): the callback times the driver
// loop kept, the counters the session and the runner kept, one RESULT line
// and the exit code. Counts decide the exit; time decides it only under
// --strict-time, so CI gates on what is deterministic.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::bench {

struct Percentiles {
    uint64_t p50_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t max_ns = 0;
};

// Nearest-rank percentiles of `ns` (copied and sorted; `ns` is left as is).
Percentiles percentiles(const std::vector<uint64_t>& ns);

// Callbacks of `ns` strictly over `budget_ns`.
uint64_t over_budget(const std::vector<uint64_t>& ns, uint64_t budget_ns);

struct BenchCounts {
    uint64_t callbacks = 0;
    uint64_t budget_ns = 0;
    uint64_t misses = 0;       // callbacks over budget
    uint64_t host_misses = 0;  // chain blocks a host returned late, past the first callback
    uint64_t drops = 0;        // node blocks played silent or dropped (ADR-0063 and the runner)
    uint64_t allocs = 0;       // heap calls on the thread inside process()
    int64_t lag = 0;           // best lag of the 2048-frame track against the dry one
    double align_error = 0.0;  // worst sample error at that lag
    uint32_t compensation = 0; // frames the session delays the dry track by
    bool aligned_compared = false;
};

struct Verdict {
    int exit_code = 0;
    // Each reason the run failed, comma separated ("drops,misses"); "none".
    std::string reasons;
};

// Exit 1 on drops, allocations, a nonzero lag, an alignment error over
// `tolerance` or no alignment window compared; with `strict_time`, also on
// a miss or a host miss.
Verdict verdict(const BenchCounts& c, bool strict_time, double tolerance);

// "RESULT callbacks=... p50_us=... p99_us=... max_us=... budget_us=...
// misses=... host_miss=... drops=... allocs=... lag=... align_err=...
// comp=... verdict=PASS|FAIL reasons=...": one line, keys fixed, so a
// script (and the smoke test) reads it.
std::string result_line(const BenchCounts& c, const Percentiles& p, const Verdict& v);

}  // namespace sonora::bench
