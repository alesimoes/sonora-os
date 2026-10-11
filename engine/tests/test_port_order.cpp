// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/port_order.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    using sonora::natural_less;

    // --- the case that matters: a lexicographic sort gets this wrong ---
    {
        CHECK(natural_less("capture_AUX2", "capture_AUX10"));
        CHECK(!natural_less("capture_AUX10", "capture_AUX2"));
    }

    // --- an 18-channel interface sorts into channel order ---
    {
        std::vector<std::string> ports;
        for (int i = 17; i >= 0; --i) {
            ports.push_back("alsa_input.usb-BEHRINGER:capture_AUX" +
                            std::to_string(i));
        }
        std::sort(ports.begin(), ports.end(), natural_less);
        for (int i = 0; i < 18; ++i) {
            const std::string want =
                "alsa_input.usb-BEHRINGER:capture_AUX" + std::to_string(i);
            if (ports[static_cast<std::size_t>(i)] != want) {
                std::fprintf(stderr, "FAIL at %d: got %s want %s\n", i,
                             ports[static_cast<std::size_t>(i)].c_str(),
                             want.c_str());
                return 1;
            }
        }
    }

    // --- zero padding must not change the order ---
    {
        CHECK(natural_less("in_02", "in_10"));
        CHECK(natural_less("in_2", "in_010"));
        // Equal numeric value, different width: not a crash, and consistent.
        CHECK(natural_less("in_02", "in_2") == !natural_less("in_2", "in_02") ||
              (!natural_less("in_02", "in_2") && !natural_less("in_2", "in_02")));
    }

    // --- names without digits fall back to plain comparison ---
    {
        CHECK(natural_less("capture_FL", "capture_FR"));
        CHECK(!natural_less("capture_FR", "capture_FL"));
    }

    // --- a prefix sorts before the longer name ---
    {
        CHECK(natural_less("in_1", "in_1_extra"));
        CHECK(!natural_less("in_1_extra", "in_1"));
    }

    // --- strict weak ordering: nothing is less than itself ---
    {
        CHECK(!natural_less("capture_AUX7", "capture_AUX7"));
        CHECK(!natural_less("", ""));
    }

    // --- multiple numeric runs compare left to right ---
    {
        CHECK(natural_less("card1_port9", "card1_port10"));
        CHECK(natural_less("card1_port99", "card2_port1"));
    }

    // --- a sort over mixed names is stable and total ---
    {
        std::vector<std::string> ports = {
            "sys:capture_12", "sys:capture_1", "sys:capture_3",
            "sys:capture_20", "sys:capture_2",
        };
        std::sort(ports.begin(), ports.end(), natural_less);
        const std::vector<std::string> want = {
            "sys:capture_1", "sys:capture_2", "sys:capture_3",
            "sys:capture_12", "sys:capture_20",
        };
        CHECK(ports == want);
    }

    std::printf("test_port_order: ok\n");
    return 0;
}
