// SPDX-License-Identifier: GPL-3.0-or-later
//
// BackendConfig::capture_device chooses which device's ports become the
// session's inputs. The default policy, revised by the owner 2026-10-13: an
// empty name means the one USB audio interface. While the startup wait runs,
// nothing else is chosen, since an interface can enumerate late. Once it is
// over with no USB device, the one non-USB device (the laptop's onboard
// audio) is used: until the interface is connected, onboard is what the
// owner develops on. Several of either, and no name, mean asking which.
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

// The interface not enumerated yet and the wait still running: onboard is
// not chosen yet. Nothing is selected, so the caller keeps waiting.
int test_empty_name_waits_before_falling_back() {
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

// The owner's case, 2026-10-13: the laptop alone. The wait is over and no
// interface came, so its onboard capture is recorded from.
int test_after_the_wait_the_one_onboard_device_is_used() {
    const std::vector<PortInfo> onboard = {port("hda", 0, false), port("hda", 1, false)};
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(onboard, "", out, error, true));
    CHECK(error.empty());
    CHECK(out.size() == 2);
    CHECK(out[0].device == "hda");
    CHECK(out[1].channel == 1);
    return 0;
}

// The interface still wins over onboard after the wait.
int test_after_the_wait_usb_is_still_preferred() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(two_cards(), "", out, error, true));
    CHECK(out.size() == 3);
    for (const PortInfo& p : out) CHECK(p.device == "umc");
    return 0;
}

// Onboard plus HDMI capture, say: guessing between them could record the
// wrong one, so the operator is asked, with every candidate named.
int test_after_the_wait_several_onboard_devices_are_named() {
    const std::vector<PortInfo> two = {port("hda", 0, false), port("hda", 1, false),
                                       port("hdmi", 0, false)};
    std::vector<PortInfo> out = {port("stale", 0, true)};
    std::string error;
    CHECK(!select_device(two, "", out, error, true));
    CHECK(out.empty());
    CHECK(error.find("\"hda\"") != std::string::npos);
    CHECK(error.find("\"hdmi\"") != std::string::npos);
    CHECK(error.find("--device") != std::string::npos);
    return 0;
}

// Nothing at all after the wait is still not an error: the caller keeps
// looking and says why on screen.
int test_after_the_wait_nothing_present_selects_nothing() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device({}, "", out, error, true));
    CHECK(out.empty());
    CHECK(error.empty());
    return 0;
}

// A name is a name: the wait being over changes nothing about it.
int test_after_the_wait_a_named_device_is_unchanged() {
    std::vector<PortInfo> out;
    std::string error;
    CHECK(select_device(two_cards(), "umc2", out, error, true));
    CHECK(out.empty());
    CHECK(error.empty());
    CHECK(select_device(two_cards(), "hda", out, error, true));
    CHECK(out.size() == 2);
    return 0;
}

}  // namespace

int main() {
    if (test_named_device() != 0) return 1;
    if (test_named_onboard_device() != 0) return 1;
    if (test_empty_name_prefers_the_usb_device() != 0) return 1;
    if (test_empty_name_waits_before_falling_back() != 0) return 1;
    if (test_empty_name_with_several_usb_devices_names_them() != 0) return 1;
    if (test_empty_name_with_one_usb_device() != 0) return 1;
    if (test_absent_named_device_is_described() != 0) return 1;
    if (test_absent_usb_device_is_described() != 0) return 1;
    if (test_absent_device_selects_nothing() != 0) return 1;
    if (test_after_the_wait_the_one_onboard_device_is_used() != 0) return 1;
    if (test_after_the_wait_usb_is_still_preferred() != 0) return 1;
    if (test_after_the_wait_several_onboard_devices_are_named() != 0) return 1;
    if (test_after_the_wait_nothing_present_selects_nothing() != 0) return 1;
    if (test_after_the_wait_a_named_device_is_unchanged() != 0) return 1;
    std::printf("test_select_device: ok\n");
    return 0;
}
