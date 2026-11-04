// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel names for the UMC1820. The order is the claim under test: S/PDIF
// sits at 9-10 on the way in and 11-12 on the way out, ADAT after it, so a
// picker that put ADAT at 9-16 would label channel 10 "ADAT 2".
#include <cstdio>
#include <string>

#include "sonora/device_profile.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

using sonora::backend::channel_label;
using sonora::backend::Direction;
using sonora::backend::is_umc1820;

int main() {
    CHECK(is_umc1820("UMC1820 Pro"));
    CHECK(is_umc1820("BEHRINGER UMC1820"));
    CHECK(is_umc1820("usb umc1820"));
    CHECK(!is_umc1820("UMC202HD"));
    CHECK(!is_umc1820(""));

    const std::string d = "UMC1820 Pro";
    CHECK(channel_label(d, Direction::capture, 0) == "Mic/Line 1");
    CHECK(channel_label(d, Direction::capture, 7) == "Mic/Line 8");
    CHECK(channel_label(d, Direction::capture, 8) == "S/PDIF L");
    CHECK(channel_label(d, Direction::capture, 9) == "S/PDIF R");
    CHECK(channel_label(d, Direction::capture, 10) == "ADAT 1");
    CHECK(channel_label(d, Direction::capture, 17) == "ADAT 8");
    CHECK(channel_label(d, Direction::capture, 18).empty());

    CHECK(channel_label(d, Direction::playback, 0) == "Out 1");
    CHECK(channel_label(d, Direction::playback, 9) == "Out 10");
    CHECK(channel_label(d, Direction::playback, 10) == "S/PDIF L");
    CHECK(channel_label(d, Direction::playback, 11) == "S/PDIF R");
    CHECK(channel_label(d, Direction::playback, 12) == "ADAT 1");
    CHECK(channel_label(d, Direction::playback, 19) == "ADAT 8");
    CHECK(channel_label(d, Direction::playback, 20).empty());

    // Unknown devices get no label rather than a wrong one.
    CHECK(channel_label("Built-in Audio", Direction::capture, 0).empty());
    std::printf("test_device_profile: ok\n");
    return 0;
}
