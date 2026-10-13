// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ports are numbered per device. PipeWire's Pro Audio profile applies to
// every ALSA card, so a machine with onboard audio and the UMC1820 offers
// both cards' capture ports. Numbered as one list, the onboard pair would
// sort into the middle of the interface's channels, or ahead of them, and
// every track after it would record its neighbour.
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/port_devices.h"

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
using sonora::drivers::describe_ports;

const std::string kUmc = "alsa_input.usb-BEHRINGER_UMC1820-00.pro-input-0";
const std::string kHda = "alsa_input.pci-0000_00_1f.3.pro-input-0";

int test_two_cards_are_numbered_apart() {
    std::vector<std::string> names;
    for (int i = 17; i >= 0; --i) {
        names.push_back(kUmc + ":capture_AUX" + std::to_string(i));
    }
    names.push_back(kHda + ":capture_AUX1");
    names.push_back(kHda + ":capture_AUX0");

    const std::vector<PortInfo> ports = describe_ports(names);
    CHECK(ports.size() == 20);
    int umc = 0, hda = 0;
    for (const PortInfo& p : ports) {
        if (p.device == kUmc) {
            CHECK(p.channel == static_cast<uint32_t>(umc));
            CHECK(p.name == kUmc + ":capture_AUX" + std::to_string(umc));
            ++umc;
        } else {
            CHECK(p.device == kHda);
            CHECK(p.channel == static_cast<uint32_t>(hda));
            CHECK(p.name == kHda + ":capture_AUX" + std::to_string(hda));
            ++hda;
        }
    }
    CHECK(umc == 18);
    CHECK(hda == 2);
    return 0;
}

// Each device's ports sit together, so a caller can take one device's run.
int test_a_device_is_contiguous() {
    const std::vector<PortInfo> ports = describe_ports(
        {"b:capture_2", "a:capture_10", "b:capture_1", "a:capture_2"});
    CHECK(ports.size() == 4);
    CHECK(ports[0].name == "a:capture_2");
    CHECK(ports[1].name == "a:capture_10");
    CHECK(ports[2].name == "b:capture_1");
    CHECK(ports[3].name == "b:capture_2");
    CHECK(ports[1].channel == 1);
    CHECK(ports[2].channel == 0);
    return 0;
}

// The device is the client part, before the first ':'; a port name may
// hold more colons after it.
int test_device_is_before_the_first_colon() {
    const std::vector<PortInfo> ports = describe_ports({"system:capture:1"});
    CHECK(ports.size() == 1);
    CHECK(ports[0].device == "system");
    CHECK(ports[0].name == "system:capture:1");
    return 0;
}

// The bus is asked per port and carried on every port of its device, so
// select_device() can choose the interface over onboard audio.
int test_usb_is_carried_per_port() {
    const std::vector<PortInfo> ports = describe_ports(
        {"UMC1820 Pro:capture_AUX1", "Built-in Audio Pro:capture_AUX0",
         "UMC1820 Pro:capture_AUX0"},
        [](const std::string& name) { return name.rfind("UMC1820", 0) == 0; });
    CHECK(ports.size() == 3);
    CHECK(ports[0].device == "Built-in Audio Pro" && !ports[0].usb);
    CHECK(ports[1].name == "UMC1820 Pro:capture_AUX0" && ports[1].usb);
    CHECK(ports[2].name == "UMC1820 Pro:capture_AUX1" && ports[2].usb);
    // Without a way to tell, nothing is USB.
    CHECK(!describe_ports({"UMC1820 Pro:capture_AUX0"})[0].usb);
    return 0;
}

}  // namespace

int main() {
    if (test_two_cards_are_numbered_apart() != 0) return 1;
    if (test_a_device_is_contiguous() != 0) return 1;
    if (test_device_is_before_the_first_colon() != 0) return 1;
    if (test_usb_is_carried_per_port() != 0) return 1;
    std::printf("test_port_devices: ok\n");
    return 0;
}
