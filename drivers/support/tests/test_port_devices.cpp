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

// Two UMC1820s share the description "UMC1820 Pro", so pw-jack 1.4.2 gives
// both one client name and the second node's ports "-<global id>"
// (measured, brain/research/multichannel-usb-interfaces.md). The ids come
// in creation order, so which unit is second can change between boots.
std::vector<std::string> two_umcs(const char* suffix) {
    std::vector<std::string> names;
    for (int i = 17; i >= 0; --i) {
        names.push_back("UMC1820 Pro:capture_AUX" + std::to_string(i));
        names.push_back("UMC1820 Pro:capture_AUX" + std::to_string(i) + suffix);
    }
    return names;
}

// The key alsa_card_unit() gives: serial, then bus path. `a_first` says
// whether unit A, serial "A", got the bare names this boot.
std::string unit_of(const std::string& name, bool a_first) {
    const bool suffixed = name.find('-') != std::string::npos;
    const bool is_a = suffixed != a_first;
    return is_a ? std::string("A\x01" "1-4:1.0") : std::string("B\x01" "1-3:1.0");
}

// Each unit is its own device, numbered from channel 0, and the unit keeps
// its device name and channel order whichever of them enumerated first.
int test_two_units_of_one_model_are_two_devices_on_every_boot() {
    for (const bool a_first : {true, false}) {
        const char* suffix = a_first ? "-48" : "-112";
        const std::vector<PortInfo> ports = describe_ports(
            two_umcs(suffix), nullptr,
            [a_first](const std::string& n) { return unit_of(n, a_first); });
        CHECK(ports.size() == 36);
        for (uint32_t k = 0; k < 18; ++k) {
            const PortInfo& a = ports[k];
            const PortInfo& b = ports[18 + k];
            CHECK(a.device == "UMC1820 Pro #1");
            CHECK(b.device == "UMC1820 Pro #2");
            CHECK(a.channel == k && b.channel == k);
            CHECK(unit_of(a.name, a_first)[0] == 'A');
            CHECK(unit_of(b.name, a_first)[0] == 'B');
            const std::string aux = "UMC1820 Pro:capture_AUX" + std::to_string(k);
            CHECK(a.name == (a_first ? aux : aux + suffix));
            CHECK(b.name == (a_first ? aux + suffix : aux));
        }
    }
    return 0;
}

// With two interfaces and no --device, the default policy names both and
// records from neither, rather than taking one unit's ports, or both.
int test_two_units_and_no_name_is_refused() {
    const std::vector<PortInfo> ports = describe_ports(
        two_umcs("-48"), [](const std::string&) { return true; },
        [](const std::string& n) { return unit_of(n, true); });
    std::vector<PortInfo> chosen;
    std::string error;
    CHECK(!sonora::backend::select_device(ports, "", chosen, error));
    CHECK(error.find("\"UMC1820 Pro #1\"") != std::string::npos);
    CHECK(error.find("\"UMC1820 Pro #2\"") != std::string::npos);
    CHECK(sonora::backend::select_device(ports, "UMC1820 Pro #2", chosen, error));
    CHECK(chosen.size() == 18);
    for (const PortInfo& p : chosen) CHECK(unit_of(p.name, true)[0] == 'B');
    return 0;
}

// Without a key (jackd2, or aliases off) the suffix still keeps the units
// apart: the bare names are one, the suffixed ones the other, and a
// channel never comes from the wrong unit.
int test_without_a_key_the_suffix_splits_the_units() {
    const std::vector<PortInfo> ports = describe_ports(two_umcs("-48"));
    CHECK(ports.size() == 36);
    for (uint32_t k = 0; k < 18; ++k) {
        const std::string aux = "UMC1820 Pro:capture_AUX" + std::to_string(k);
        CHECK(ports[k].device == "UMC1820 Pro #1");
        CHECK(ports[k].name == aux);
        CHECK(ports[k].channel == k);
        CHECK(ports[18 + k].device == "UMC1820 Pro #2");
        CHECK(ports[18 + k].name == aux + "-48");
        CHECK(ports[18 + k].channel == k);
    }
    return 0;
}

// A name that ends in "-<digits>" of its own accord, with no bare twin, is
// not a second unit; and a client with one unit keeps its bare name.
int test_a_lone_dash_number_is_not_a_unit() {
    const std::vector<PortInfo> ports =
        describe_ports({"Synth:out-2", "Synth:out-1"}, nullptr,
                       [](const std::string&) { return std::string(); });
    CHECK(ports.size() == 2);
    CHECK(ports[0].device == "Synth" && ports[1].device == "Synth");
    CHECK(ports[0].name == "Synth:out-1" && ports[0].channel == 0);
    CHECK(ports[1].name == "Synth:out-2" && ports[1].channel == 1);
    const std::vector<PortInfo> one = describe_ports(
        {"UMC1820 Pro:capture_AUX1", "UMC1820 Pro:capture_AUX0"}, nullptr,
        [](const std::string&) { return std::string("A\x01" "1-4:1.0"); });
    CHECK(one[0].device == "UMC1820 Pro" && one[1].device == "UMC1820 Pro");
    return 0;
}

}  // namespace

int main() {
    if (test_two_cards_are_numbered_apart() != 0) return 1;
    if (test_a_device_is_contiguous() != 0) return 1;
    if (test_device_is_before_the_first_colon() != 0) return 1;
    if (test_usb_is_carried_per_port() != 0) return 1;
    if (test_without_a_key_the_suffix_splits_the_units() != 0) return 1;
    if (test_a_lone_dash_number_is_not_a_unit() != 0) return 1;
    if (test_two_units_of_one_model_are_two_devices_on_every_boot() != 0) return 1;
    if (test_two_units_and_no_name_is_refused() != 0) return 1;
    std::printf("test_port_devices: ok\n");
    return 0;
}
