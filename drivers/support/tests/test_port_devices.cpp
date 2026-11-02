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

// Two units of one model, as pipewire-jack lists them (measured: "Dup
// Pro:capture_AUX1" and "Dup Pro:capture_AUX1-48"): one client name, the
// later unit's ports suffixed with their own ids. Two devices, each in
// channel order, never one device with the units interleaved.
int test_second_unit_is_its_own_device() {
    std::vector<std::string> names;
    for (int c = 17; c >= 0; --c) {
        names.push_back("UMC1820 Pro:capture_AUX" + std::to_string(c));
        names.push_back("UMC1820 Pro:capture_AUX" + std::to_string(c) + "-" +
                        std::to_string(80 + c));
    }
    const std::vector<PortInfo> ports = describe_ports(names);
    CHECK(ports.size() == 36);
    for (uint32_t c = 0; c < 18; ++c) {
        const PortInfo& first = ports[c];
        const PortInfo& second = ports[18 + c];
        CHECK(first.device == "UMC1820 Pro");
        CHECK(first.name == "UMC1820 Pro:capture_AUX" + std::to_string(c));
        CHECK(first.channel == c);
        CHECK(second.device == "UMC1820 Pro (2)");
        CHECK(second.name ==
              "UMC1820 Pro:capture_AUX" + std::to_string(c) + "-" + std::to_string(80 + c));
        CHECK(second.channel == c);
    }
    return 0;
}

// A third unit collides with the first too; its ports carry later ids than
// the second's, as the server created it later.
int test_units_follow_id_order() {
    const std::vector<PortInfo> ports = describe_ports(
        {"Dup Pro:capture_AUX0-70", "Dup Pro:capture_AUX1-71", "Dup Pro:capture_AUX0",
         "Dup Pro:capture_AUX1", "Dup Pro:capture_AUX0-48", "Dup Pro:capture_AUX1-49"});
    CHECK(ports.size() == 6);
    CHECK(ports[0].device == "Dup Pro" && ports[0].name == "Dup Pro:capture_AUX0");
    CHECK(ports[2].device == "Dup Pro (2)" && ports[2].name == "Dup Pro:capture_AUX0-48");
    CHECK(ports[3].device == "Dup Pro (2)" && ports[3].channel == 1);
    CHECK(ports[4].device == "Dup Pro (3)" && ports[4].name == "Dup Pro:capture_AUX0-70");
    CHECK(ports[5].device == "Dup Pro (3)" && ports[5].channel == 1);
    return 0;
}

// A dash and digits are a unit's suffix only when the bare name exists:
// otherwise they are the port's own name, or the first unit is gone and
// the second is the only one left.
int test_a_name_ending_in_digits_is_not_a_unit() {
    const std::vector<PortInfo> ports = describe_ports(
        {"Mixer-2:capture_1-2", "Mixer-2:capture_1-3", "Dup Pro:capture_AUX0-48"});
    CHECK(ports.size() == 3);
    CHECK(ports[0].device == "Dup Pro" && ports[0].channel == 0);
    CHECK(ports[1].device == "Mixer-2" && ports[1].channel == 0);
    CHECK(ports[2].device == "Mixer-2" && ports[2].channel == 1);
    return 0;
}

// Separate JACK clients with one description: pipewire-jack suffixes the
// later client instead (measured: "Dup Pro-46:capture_AUX0"). Same naming.
int test_client_suffix_is_a_unit_too() {
    const std::vector<PortInfo> ports = describe_ports(
        {"Dup Pro-46:capture_AUX1", "Dup Pro:capture_AUX0", "Dup Pro-46:capture_AUX0",
         "Dup Pro:capture_AUX1", "Dup Pro-60:capture_AUX0", "Solo-3:capture_AUX0"});
    CHECK(ports.size() == 6);
    CHECK(ports[0].device == "Dup Pro" && ports[1].device == "Dup Pro");
    CHECK(ports[2].device == "Dup Pro (2)" && ports[2].name == "Dup Pro-46:capture_AUX0");
    CHECK(ports[3].device == "Dup Pro (2)" && ports[3].channel == 1);
    CHECK(ports[4].device == "Dup Pro (3)" && ports[4].name == "Dup Pro-60:capture_AUX0");
    // No client "Solo" exists: the dash and digits are its own name.
    CHECK(ports[5].device == "Solo-3");
    return 0;
}

// Both forms at once: the client copy ranks after the port copies' unit.
int test_both_suffix_forms() {
    const std::vector<PortInfo> ports =
        describe_ports({"Dup Pro:capture_AUX0", "Dup Pro:capture_AUX0-48",
                        "Dup Pro-70:capture_AUX0"});
    CHECK(ports.size() == 3);
    CHECK(ports[0].device == "Dup Pro");
    CHECK(ports[1].device == "Dup Pro (2)" && ports[1].name == "Dup Pro:capture_AUX0-48");
    CHECK(ports[2].device == "Dup Pro (3)" && ports[2].name == "Dup Pro-70:capture_AUX0");
    return 0;
}

// Two USB units under one name are two USB devices.
int test_usb_units_stay_usb() {
    const std::vector<PortInfo> ports = describe_ports(
        {"UMC1820 Pro:capture_AUX0", "UMC1820 Pro:capture_AUX0-48"},
        [](const std::string&) { return true; });
    CHECK(ports.size() == 2);
    CHECK(ports[0].usb && ports[1].usb);
    CHECK(ports[0].device != ports[1].device);
    return 0;
}

}  // namespace

int main() {
    if (test_two_cards_are_numbered_apart() != 0) return 1;
    if (test_a_device_is_contiguous() != 0) return 1;
    if (test_device_is_before_the_first_colon() != 0) return 1;
    if (test_usb_is_carried_per_port() != 0) return 1;
    if (test_second_unit_is_its_own_device() != 0) return 1;
    if (test_units_follow_id_order() != 0) return 1;
    if (test_a_name_ending_in_digits_is_not_a_unit() != 0) return 1;
    if (test_usb_units_stay_usb() != 0) return 1;
    if (test_client_suffix_is_a_unit_too() != 0) return 1;
    if (test_both_suffix_forms() != 0) return 1;
    std::printf("test_port_devices: ok\n");
    return 0;
}
