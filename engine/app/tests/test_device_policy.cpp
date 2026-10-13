// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the engine does about its capture device around the startup wait,
// against a stub backend. The default policy, revised by the owner
// 2026-10-13 (ADR-0007 M2 note, item 8): the USB interface when there is
// one; after the wait, the one onboard device, said on screen; several and
// none named, a failure that lists them; an interface that appears later is
// noticed but not switched to.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"
#include "sonora/device_policy.h"

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

PortInfo port(const std::string& device, uint32_t channel, bool usb) {
    PortInfo p{device, device + ":capture_AUX" + std::to_string(channel), channel};
    p.usb = usb;
    return p;
}

// Enough of an AudioBackend to drive the policy: the ports the server
// offers, which inputs are connected, and the device chosen after open().
class StubBackend final : public sonora::backend::AudioBackend {
public:
    std::vector<PortInfo> ports;
    std::vector<bool> connected = std::vector<bool>(8, false);
    std::string chosen;

    std::vector<PortInfo> physical_capture_ports() override { return ports; }
    std::vector<PortInfo> physical_playback_ports() override { return {}; }
    bool open(const sonora::backend::BackendConfig&,
              sonora::backend::ProcessCallback&, std::string&) override {
        return true;
    }
    bool activate(std::string&) override { return true; }
    int connect_inputs_in_order() override { return 0; }
    int connected_input_count() const override {
        int n = 0;
        for (const bool c : connected) n += c ? 1 : 0;
        return n;
    }
    bool input_connected(uint32_t index) const override {
        return index < connected.size() && connected[index];
    }
    bool choose_capture_device(const std::string& device) override {
        if (!chosen.empty()) return false;
        chosen = device;
        return true;
    }
    void deactivate() noexcept override {}
    uint32_t sample_rate() const override { return 48000; }
    uint32_t buffer_frames() const override { return 128; }
    uint32_t xruns() const override { return 0; }
    uint32_t clock_discontinuities() const override { return 0; }
    bool fatal() const override { return false; }
    std::string fatal_reason() const override { return {}; }
};

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

// The laptop alone: after the wait its onboard capture is chosen in the
// backend, and the notice says so in the words the screen keeps showing.
int test_onboard_is_chosen_after_the_wait() {
    StubBackend audio;
    audio.ports = {port("Built-in Audio Pro", 0, false),
                   port("Built-in Audio Pro", 1, false)};
    const sonora::Fallback f = sonora::fall_back_after_wait(audio);
    CHECK(f.ok);
    CHECK(f.device == "Built-in Audio Pro");
    CHECK(audio.chosen == "Built-in Audio Pro");
    CHECK(f.notice ==
          "recording from Built-in Audio Pro (onboard): no USB interface found");
    return 0;
}

// Two non-USB devices: no guess. The error names both, and nothing is
// chosen in the backend.
int test_several_onboard_devices_fail_listing_them() {
    StubBackend audio;
    audio.ports = {port("Built-in Audio Pro", 0, false), port("HDMI Audio Pro", 0, false)};
    const sonora::Fallback f = sonora::fall_back_after_wait(audio);
    CHECK(!f.ok);
    CHECK(contains(f.error, "Built-in Audio Pro"));
    CHECK(contains(f.error, "HDMI Audio Pro"));
    CHECK(audio.chosen.empty());
    return 0;
}

// The interface arrived at the last moment: it is the device, and there is
// nothing to announce.
int test_usb_at_the_end_of_the_wait_is_no_fallback() {
    StubBackend audio;
    audio.ports = {port("Built-in Audio Pro", 0, false), port("UMC1820 Pro", 0, true)};
    const sonora::Fallback f = sonora::fall_back_after_wait(audio);
    CHECK(f.ok);
    CHECK(f.device == "UMC1820 Pro");
    CHECK(f.notice.empty());
    return 0;
}

// Nothing at all: not a failure, so the engine stays up and keeps looking
// rather than exiting into a restart loop.
int test_nothing_present_is_not_a_failure() {
    StubBackend audio;
    const sonora::Fallback f = sonora::fall_back_after_wait(audio);
    CHECK(f.ok);
    CHECK(f.device.empty());
    CHECK(f.notice.empty());
    CHECK(audio.chosen.empty());
    return 0;
}

// The interface was connected from and then unpatched by hand: the backend
// keeps it, so onboard is neither chosen nor announced.
int test_a_backend_with_a_device_is_left_alone() {
    StubBackend audio;
    audio.ports = {port("Built-in Audio Pro", 0, false)};
    audio.chosen = "UMC1820 Pro";
    const sonora::Fallback f = sonora::fall_back_after_wait(audio);
    CHECK(f.ok);
    CHECK(f.device.empty());
    CHECK(f.notice.empty());
    CHECK(audio.chosen == "UMC1820 Pro");
    return 0;
}

// Plugged in while the engine runs on onboard: suggest a restart, since
// switching would change the channel count under the session.
int test_a_late_usb_interface_suggests_a_restart() {
    const std::vector<PortInfo> before = {port("Built-in Audio Pro", 0, false)};
    CHECK(sonora::late_usb_notice(before).empty());
    const std::vector<PortInfo> after = {port("Built-in Audio Pro", 0, false),
                                         port("UMC1820 Pro", 0, true)};
    const std::string n = sonora::late_usb_notice(after);
    CHECK(contains(n, "UMC1820 Pro"));
    CHECK(contains(n, "restart"));
    return 0;
}

// The review's low: some inputs connected, but none on an armed track, was
// reported as "nothing is connected".
int test_why_no_inputs_with_some_connected() {
    StubBackend audio;
    audio.ports = {port("Built-in Audio Pro", 0, false),
                   port("Built-in Audio Pro", 1, false)};
    audio.connected[0] = true;
    audio.connected[1] = true;
    const std::string why = sonora::why_no_inputs(audio, "Built-in Audio Pro", 8);
    CHECK(!contains(why, "nothing is connected"));
    CHECK(contains(why, "2 of 8 inputs"));
    CHECK(contains(why, "armed"));
    return 0;
}

int test_why_no_inputs_with_none_connected() {
    StubBackend audio;
    audio.ports = {port("Built-in Audio Pro", 0, false)};
    const std::string why = sonora::why_no_inputs(audio, "Built-in Audio Pro", 8);
    CHECK(contains(why, "nothing is connected"));
    // No device named and none USB: why_no_inputs says what is there.
    CHECK(contains(sonora::why_no_inputs(audio, "", 8), "no USB audio interface"));
    return 0;
}

bool write_file(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fputs(text.c_str(), f);
    return std::fclose(f) == 0;
}

// The operator's choice, kept across reboots in a file, so the appliance's
// unit need not be edited. The first file that names a device wins.
int test_device_setting_file() {
    char dir[] = "/tmp/sonora-device-XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string home = std::string(dir) + "/home";
    const std::vector<std::string> paths =
        sonora::device_setting_paths(home.c_str(), dir);
    CHECK(paths.size() == 2);
    CHECK(paths[0] == home + "/.config/sonora/device");
    CHECK(paths[1] == std::string(dir) + "/device");

    std::string from;
    CHECK(sonora::read_device_setting(paths, &from).empty());
    CHECK(from.empty());

    CHECK(write_file(paths[1], "# the one to record from\n  Built-in Audio Pro \n"));
    CHECK(sonora::read_device_setting(paths, &from) == "Built-in Audio Pro");
    CHECK(from == paths[1]);

    CHECK(::mkdir(home.c_str(), 0700) == 0);
    CHECK(::mkdir((home + "/.config").c_str(), 0700) == 0);
    CHECK(::mkdir((home + "/.config/sonora").c_str(), 0700) == 0);
    // Comments and blank lines only: says nothing, so the next file speaks.
    CHECK(write_file(paths[0], "\n# nothing chosen\n\n"));
    CHECK(sonora::read_device_setting(paths, &from) == "Built-in Audio Pro");
    CHECK(write_file(paths[0], "UMC1820 Pro\r\n"));
    CHECK(sonora::read_device_setting(paths, &from) == "UMC1820 Pro");
    CHECK(from == paths[0]);

    std::remove(paths[0].c_str());
    std::remove(paths[1].c_str());
    ::rmdir((home + "/.config/sonora").c_str());
    ::rmdir((home + "/.config").c_str());
    ::rmdir(home.c_str());
    ::rmdir(dir);
    return 0;
}

}  // namespace

int main() {
    if (test_onboard_is_chosen_after_the_wait() != 0) return 1;
    if (test_several_onboard_devices_fail_listing_them() != 0) return 1;
    if (test_usb_at_the_end_of_the_wait_is_no_fallback() != 0) return 1;
    if (test_nothing_present_is_not_a_failure() != 0) return 1;
    if (test_a_backend_with_a_device_is_left_alone() != 0) return 1;
    if (test_a_late_usb_interface_suggests_a_restart() != 0) return 1;
    if (test_why_no_inputs_with_some_connected() != 0) return 1;
    if (test_why_no_inputs_with_none_connected() != 0) return 1;
    if (test_device_setting_file() != 0) return 1;
    std::printf("test_device_policy: ok\n");
    return 0;
}
