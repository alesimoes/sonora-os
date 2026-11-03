// SPDX-License-Identifier: GPL-3.0-or-later
// Regression (BACKLOG "Wall-clock bars under the sanitizers"; CI on PR #66):
// a sanitizer slows code by a factor nobody knows, so a wall-clock bar
// measured under one says nothing about the code. The tube suite's 2.67 ms
// worst-block bar read 3097.7 us under ASan and UBSan on a GitHub runner and
// failed unrelated PRs. Under a sanitizer a time bar is reported, not held;
// in every other build it is held, and a correctness bar is held in both.
#include <cstdio>

#include "sonora/measure/measure.h"

namespace {

// The compiler's own word, where it gives one, that this translation unit is
// instrumented. The harness reads a CMake define instead (it also sees
// UBSan, which GCC announces with no macro), so the two must agree.
bool compiler_says_sanitized() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int run_suite(bool time_over, bool correctness_over, const char* quality) {
    sonora::measure::Args args;
    args.quality_path = quality;
    sonora::measure::Suite s("time_bars", [] { return nullptr; }, args);
    s.check_time_le("worst block", time_over ? 3097.7 : 100.0, 2667.0, "us", "the 128-frame period at 48 kHz");
    s.check_le("peak output", correctness_over ? 9.0 : 1.0, 8.06, "FS");
    return s.finish();
}

// Regression (#312's loaded full-suite run: native_fet's high quality read a
// worst block of 780 us against the 667 us period on a host loaded by the
// parallel suite, the code unchanged). A period bar is held to the period on a
// machine as fast as the reference one, stretched by the slowdown the
// reference kernel measures on a slower or loaded one, and never tightened.
int run_period(double block_us, double kernel_us, const char* quality) {
    sonora::measure::Args args;
    args.quality_path = quality;
    sonora::measure::Suite s("time_bars", [] { return nullptr; }, args);
    s.check_period_le("worst block", block_us, 667.0, kernel_us, "the 128-frame period at 192 kHz");
    return s.finish();
}

void period_bars(const char* quality, bool sanitized) {
    const double ref = sonora::measure::reference_kernel_reference_us();
    expect(run_period(600.0, ref, quality) == 0, "period bar: inside the period at the reference speed passes");
    // A real overrun is still caught where nothing stretches the bar; under
    // a sanitizer every time bar is reported.
    expect(run_period(780.0, ref, quality) == (sanitized ? 0 : 1),
           "period bar: over the period at the reference speed fails");
    expect(run_period(780.0, 1.5 * ref, quality) == 0, "period bar: a host 1.5x slow stretches the bar to 1000 us");
    expect(run_period(1100.0, 1.5 * ref, quality) == (sanitized ? 0 : 1),
           "period bar: beyond the stretched bar fails");
    expect(run_period(700.0, 0.5 * ref, quality) == (sanitized ? 0 : 1),
           "period bar: a host faster than the reference holds the period itself");
    expect(run_period(600.0, 0.5 * ref, quality) == 0, "period bar: and does not tighten it");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_time_bars <quality.json>\n");
        return 2;
    }
    const bool sanitized = sonora::measure::sanitized_build();
    std::printf("sanitized build (CMake): %s; compiler macros: %s\n", sanitized ? "yes" : "no",
                compiler_says_sanitized() ? "yes" : "no");
    // A compiler that reports instrumentation the CMake define missed would
    // hold the time bars under a sanitizer again.
    if (compiler_says_sanitized()) expect(sanitized, "the CMake define sees the compiler's sanitizer");

    expect(run_suite(false, false, argv[1]) == 0, "everything within its bar passes");
    const int time_over = run_suite(true, false, argv[1]);
    if (sanitized) {
        expect(time_over == 0, "a time bar exceeded under a sanitizer is reported, not failed");
    } else {
        expect(time_over == 1, "a time bar exceeded in an uninstrumented build fails");
    }
    expect(run_suite(false, true, argv[1]) == 1, "a correctness bar exceeded fails in every build");
    period_bars(argv[1], sanitized);
    return failures == 0 ? 0 : 1;
}
