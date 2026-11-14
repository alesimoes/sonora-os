// SPDX-License-Identifier: GPL-3.0-or-later
#include "bench_report.h"

#include <algorithm>
#include <cstdio>

namespace sonora::bench {

namespace {

uint64_t rank(const std::vector<uint64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    // Nearest rank: the smallest value with at least q of the samples at or
    // below it, so p99 of 20000 callbacks is the 19800th, never interpolated.
    auto k = static_cast<std::size_t>(q * static_cast<double>(sorted.size()) + 0.999999);
    if (k < 1) k = 1;
    if (k > sorted.size()) k = sorted.size();
    return sorted[k - 1];
}

void add_reason(std::string& r, const char* what) {
    if (!r.empty()) r += ',';
    r += what;
}

double us(uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

}  // namespace

Percentiles percentiles(const std::vector<uint64_t>& ns) {
    std::vector<uint64_t> s(ns);
    std::sort(s.begin(), s.end());
    Percentiles p;
    p.p50_ns = rank(s, 0.50);
    p.p99_ns = rank(s, 0.99);
    p.max_ns = s.empty() ? 0 : s.back();
    return p;
}

uint64_t over_budget(const std::vector<uint64_t>& ns, uint64_t budget_ns) {
    return static_cast<uint64_t>(
        std::count_if(ns.begin(), ns.end(), [budget_ns](uint64_t t) { return t > budget_ns; }));
}

Verdict verdict(const BenchCounts& c, bool strict_time, double tolerance) {
    Verdict v;
    if (c.drops != 0) add_reason(v.reasons, "drops");
    if (c.allocs != 0) add_reason(v.reasons, "allocs");
    if (!c.aligned_compared) add_reason(v.reasons, "no_alignment_window");
    if (c.lag != 0) add_reason(v.reasons, "lag");
    if (c.align_error > tolerance) add_reason(v.reasons, "align_err");
    if (strict_time && c.misses != 0) add_reason(v.reasons, "misses");
    if (strict_time && c.host_misses != 0) add_reason(v.reasons, "host_miss");
    v.exit_code = v.reasons.empty() ? 0 : 1;
    if (v.reasons.empty()) v.reasons = "none";
    return v;
}

std::string result_line(const BenchCounts& c, const Percentiles& p, const Verdict& v) {
    char line[512];
    std::snprintf(line, sizeof line,
                  "RESULT callbacks=%llu p50_us=%.1f p99_us=%.1f max_us=%.1f budget_us=%.1f misses=%llu "
                  "host_miss=%llu drops=%llu allocs=%llu lag=%lld align_err=%.3g comp=%u verdict=%s reasons=%s",
                  static_cast<unsigned long long>(c.callbacks), us(p.p50_ns), us(p.p99_ns), us(p.max_ns),
                  us(c.budget_ns), static_cast<unsigned long long>(c.misses),
                  static_cast<unsigned long long>(c.host_misses), static_cast<unsigned long long>(c.drops),
                  static_cast<unsigned long long>(c.allocs), static_cast<long long>(c.lag), c.align_error,
                  c.compensation, v.exit_code == 0 ? "PASS" : "FAIL", v.reasons.c_str());
    return line;
}

}  // namespace sonora::bench
