// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the operator reads when a backend gives up. drivers/jack and
// drivers/fake share these words, so a test against the fake asserts the
// sentence the appliance prints. Written against JackBackend::fatal_reason()
// as it stood before the text moved here: every expected string below is the
// one that function produced.
#include <cstdio>
#include <string>

#include "sonora/fatal_reason.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::drivers::describe_fatal;
using sonora::drivers::Fatal;
using sonora::drivers::FatalContext;

FatalContext context() {
    FatalContext c;
    c.rate = 48000;
    c.admitted_frames = 128;
    c.max_frames = 8192;
    return c;
}

int test_none_is_empty() {
    CHECK(describe_fatal({Fatal::None, 0}, context()).empty());
    return 0;
}

int test_server_gone() {
    CHECK(describe_fatal({Fatal::ServerGone, 0}, context()) ==
          "the audio server went away");
    return 0;
}

int test_device_gone_names_the_device_and_port() {
    FatalContext c = context();
    c.gone_port = "UMC1820 Pro:capture_AUX3";
    CHECK(describe_fatal({Fatal::DeviceGone, 0}, c) ==
          "capture device UMC1820 Pro went away (UMC1820 Pro:capture_AUX3 is gone)");
    return 0;
}

int test_device_gone_without_a_port() {
    CHECK(describe_fatal({Fatal::DeviceGone, 0}, context()) ==
          "a capture device went away");
    return 0;
}

int test_stalled() {
    CHECK(describe_fatal({Fatal::Stalled, 0}, context()) ==
          "the audio server ran no period for over a second");
    return 0;
}

int test_rate_change_gives_both_rates() {
    CHECK(describe_fatal({Fatal::RateChanged, 96000}, context()) ==
          "the audio server changed its sample rate mid-session (48000 -> 96000 Hz)");
    return 0;
}

int test_buffer_change_gives_both_sizes() {
    CHECK(describe_fatal({Fatal::BufferChanged, 256}, context()) ==
          "the audio server changed its buffer size mid-session (128 -> 256 frames)");
    return 0;
}

// No period admitted yet: the refused one was larger than the buffers.
int test_first_period_too_large() {
    FatalContext c = context();
    c.admitted_frames = 0;
    CHECK(describe_fatal({Fatal::BufferChanged, 16384}, c) ==
          "the audio server's period of 16384 frames is more than the 8192"
          " this backend can hold");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_none_is_empty();
    failed += test_server_gone();
    failed += test_device_gone_names_the_device_and_port();
    failed += test_device_gone_without_a_port();
    failed += test_stalled();
    failed += test_rate_change_gives_both_rates();
    failed += test_buffer_change_gives_both_sizes();
    failed += test_first_period_too_large();
    if (failed == 0) std::printf("fatal_reason: all tests passed\n");
    return failed == 0 ? 0 : 1;
}
