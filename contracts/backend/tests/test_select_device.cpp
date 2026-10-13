// SPDX-License-Identifier: GPL-3.0-or-later
//
// BackendConfig::capture_device chooses which device's ports become the
// session's inputs. An empty name means "the only one there is", and with
// two cards present that is a question, not a default: the engine must not
// guess between onboard audio and the interface.
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::backend::PortInfo;
using sonora::backend::select_device;

std::vector<PortInfo> two_cards() {
    return {
        {"hda", "hda:capture_AUX0", 0},
        {"hda", "hda:capture_AUX1", 1},
        {"umc", "umc:capture_AUX0", 0},
        {"umc", "umc:capture_AUX1", 1},
        {"umc", "umc:capture_AUX2", 2},
    };
}

int test_named_device() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(two_cards(), "umc", out, error));
    CHECK(out.size() == 3);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(out[i].device == "umc");
        CHECK(out[i].channel == i);
    }
    return 0;
}

int test_empty_name_with_several_devices_names_them() {
    std::vector<PortInfo> out = {{"stale", "stale:x", 0}};
    std::string error;
    CHECK(!select_device(two_cards(), "", out, error));
    CHECK(out.empty());
    CHECK(error.find("hda") != std::string::npos);
    CHECK(error.find("umc") != std::string::npos);
    return 0;
}

int test_empty_name_with_one_device() {
    std::vector<PortInfo> one = {{"umc", "umc:capture_AUX0", 0}};
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(one, "", out, error));
    CHECK(out.size() == 1);
    return 0;
}

// A USB interface can be enumerated after the engine starts. No device yet,
// or not the named one yet, selects nothing and is not an error; the caller
// sees zero ports and says so.
int test_absent_device_selects_nothing() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device({}, "", out, error));
    CHECK(out.empty());
    CHECK(select_device(two_cards(), "umc2", out, error));
    CHECK(out.empty());
    CHECK(error.empty());
    return 0;
}

}  // namespace

int main() {
    if (test_named_device() != 0) return 1;
    if (test_empty_name_with_several_devices_names_them() != 0) return 1;
    if (test_empty_name_with_one_device() != 0) return 1;
    if (test_absent_device_selects_nothing() != 0) return 1;
    std::printf("test_select_device: ok\n");
    return 0;
}
