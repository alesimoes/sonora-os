// SPDX-License-Identifier: GPL-3.0-or-later
//
// BackendConfig::capture_device chooses which device's ports become the
// session's inputs. The default policy (2026-10-12): an empty name means the
// one USB audio interface. Onboard, PCI and HDMI capture is used only when
// named: on the appliance, falling back to it records the laptop's
// microphone while the operator thinks the interface is armed. No USB
// device yet means waiting for one; several mean asking which.
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

PortInfo port(const std::string& device, uint32_t channel, bool usb) {
    PortInfo p{device, device + ":capture_AUX" + std::to_string(channel), channel};
    p.usb = usb;
    return p;
}

// Onboard audio and the interface, as on the target laptop.
std::vector<PortInfo> two_cards() {
    return {
        port("hda", 0, false),
        port("hda", 1, false),
        port("umc", 0, true),
        port("umc", 1, true),
        port("umc", 2, true),
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

// Onboard named explicitly is used like any other device.
int test_named_onboard_device() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(two_cards(), "hda", out, error));
    CHECK(out.size() == 2);
    CHECK(out[0].device == "hda");
    return 0;
}

// The case the review found: onboard plus the interface, no --device. The
// interface is chosen, not a failure and not a guess.
int test_empty_name_prefers_the_usb_device() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(two_cards(), "", out, error));
    CHECK(out.size() == 3);
    for (const PortInfo& p : out) CHECK(p.device == "umc");
    CHECK(error.empty());
    return 0;
}

// The interface not enumerated yet: onboard is not a fallback. Nothing is
// selected, so the caller waits.
int test_empty_name_never_falls_back_to_onboard() {
    const std::vector<PortInfo> onboard = {port("hda", 0, false), port("hda", 1, false)};
    std::vector<PortInfo> out = {port("stale", 0, true)};
    std::string error;
    CHECK(select_device(onboard, "", out, error));
    CHECK(out.empty());
    CHECK(error.empty());
    return 0;
}

int test_empty_name_with_several_usb_devices_names_them() {
    std::vector<PortInfo> all = two_cards();
    all.push_back(port("scarlett", 0, true));
    std::vector<PortInfo> out = {port("stale", 0, true)};
    std::string error;
    CHECK(!select_device(all, "", out, error));
    CHECK(out.empty());
    CHECK(error.find("umc") != std::string::npos);
    CHECK(error.find("scarlett") != std::string::npos);
    // Onboard is not a candidate, so naming it would only mislead.
    CHECK(error.find("hda") == std::string::npos);
    return 0;
}

int test_empty_name_with_one_usb_device() {
    const std::vector<PortInfo> one = {port("umc", 0, true)};
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(one, "", out, error));
    CHECK(out.size() == 1);
    return 0;
}

// What the operator reads when the wait for a device ends with nothing: the
// name asked for, and every device that is there, marked if USB.
int test_absent_named_device_is_described() {
    const std::string why = sonora::backend::describe_absent(two_cards(), "umc2");
    CHECK(why.find("\"umc2\"") != std::string::npos);
    CHECK(why.find("hda") != std::string::npos);
    CHECK(why.find("umc (USB)") != std::string::npos);
    return 0;
}

int test_absent_usb_device_is_described() {
    const std::vector<PortInfo> onboard = {port("hda", 0, false)};
    const std::string why = sonora::backend::describe_absent(onboard, "");
    CHECK(why.find("no USB audio interface") != std::string::npos);
    CHECK(why.find("hda") != std::string::npos);
    CHECK(why.find("--device") != std::string::npos);
    CHECK(sonora::backend::describe_absent({}, "").find("no capture device") !=
          std::string::npos);
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
    CHECK(error.empty());
    CHECK(select_device(two_cards(), "umc2", out, error));
    CHECK(out.empty());
    CHECK(error.empty());
    return 0;
}

}  // namespace

int main() {
    if (test_named_device() != 0) return 1;
    if (test_named_onboard_device() != 0) return 1;
    if (test_empty_name_prefers_the_usb_device() != 0) return 1;
    if (test_empty_name_never_falls_back_to_onboard() != 0) return 1;
    if (test_empty_name_with_several_usb_devices_names_them() != 0) return 1;
    if (test_empty_name_with_one_usb_device() != 0) return 1;
    if (test_absent_named_device_is_described() != 0) return 1;
    if (test_absent_usb_device_is_described() != 0) return 1;
    if (test_absent_device_selects_nothing() != 0) return 1;
    std::printf("test_select_device: ok\n");
    return 0;
}
