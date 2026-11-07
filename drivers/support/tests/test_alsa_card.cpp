// SPDX-License-Identifier: GPL-3.0-or-later
//
// The device policy prefers a USB interface, and the bus is not in anything
// the JACK API names. What pw-jack 1.4.2 does expose, once the client asks
// for aliases, is the port's PipeWire object.path, which carries the ALSA
// card's id; the kernel's snd-usb-audio driver gives every card it owns a
// /proc/asound/cardN/usbid. These tests pin both halves against a fake
// /proc/asound tree.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "sonora/alsa_card.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::drivers::alsa_card_is_usb;
using sonora::drivers::alsa_card_of;
using sonora::drivers::alsa_card_unit;
using sonora::drivers::with_fill_aliases;

// As measured in the container: alias 1 of a port is its object.path.
int test_card_from_object_path() {
    CHECK(alsa_card_of("alsa:acp:UMC1820:0:capture:output_0") == "UMC1820");
    CHECK(alsa_card_of("alsa:acp:PCH:0:capture:output_1") == "PCH");
    CHECK(alsa_card_of("alsa:pcm:UMC1820:0:capture:capture_0") == "UMC1820");
    // ACP falls back to the card's index when it has no id.
    CHECK(alsa_card_of("alsa:acp:2:0:capture:output_0") == "2");
    return 0;
}

int test_not_alsa_has_no_card() {
    CHECK(alsa_card_of("test-source:monitor_0").empty());
    CHECK(alsa_card_of("Built-in Audio Pro:capture_AUX0").empty());
    CHECK(alsa_card_of("alsa:acp:").empty());
    CHECK(alsa_card_of("").empty());
    // The id becomes a path under /proc/asound; nothing may climb out.
    CHECK(alsa_card_of("alsa:acp:..:0:capture").empty());
    CHECK(alsa_card_of("alsa:acp:a/b:0:capture").empty());
    return 0;
}

bool make_dir(const std::string& p) { return ::mkdir(p.c_str(), 0700) == 0; }
bool touch(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    std::fclose(f);
    return true;
}

int test_usb_is_what_the_kernel_says() {
    char tmpl[] = "/tmp/sonora-asound-XXXXXX";
    CHECK(::mkdtemp(tmpl) != nullptr);
    const std::string root = tmpl;
    // The kernel's layout: cardN directories, and a symlink per card id.
    CHECK(make_dir(root + "/card0"));
    CHECK(make_dir(root + "/card1"));
    CHECK(touch(root + "/card1/usbid"));
    CHECK(::symlink("card0", (root + "/PCH").c_str()) == 0);
    CHECK(::symlink("card1", (root + "/UMC1820").c_str()) == 0);

    CHECK(alsa_card_is_usb("UMC1820", root));
    CHECK(!alsa_card_is_usb("PCH", root));
    CHECK(alsa_card_is_usb("1", root));
    CHECK(!alsa_card_is_usb("0", root));
    CHECK(!alsa_card_is_usb("Absent", root));
    CHECK(!alsa_card_is_usb("", root));

    std::remove((root + "/card1/usbid").c_str());
    std::remove((root + "/PCH").c_str());
    std::remove((root + "/UMC1820").c_str());
    ::rmdir((root + "/card0").c_str());
    ::rmdir((root + "/card1").c_str());
    ::rmdir(root.c_str());
    return 0;
}

bool write_file(const std::string& p, const char* text) {
    std::FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    std::fputs(text, f);
    std::fclose(f);
    return true;
}

// Two units of one model: the kernel names their cards "UMC1820" and
// "UMC1820_1" in probe order, so the id says nothing about which unit is
// which. The key is read from the card's sysfs device, as the kernel lays
// it out: /sys/class/sound/cardN/device is the USB interface, whose parent
// holds the unit's serial.
int test_unit_is_the_serial_then_the_bus_path() {
    char tmpl[] = "/tmp/sonora-unit-XXXXXX";
    CHECK(::mkdtemp(tmpl) != nullptr);
    const std::string root = tmpl;
    const std::string proc = root + "/asound", sys = root + "/sound";
    const std::string usb = root + "/usb1";
    CHECK(make_dir(proc) && make_dir(sys) && make_dir(usb));
    CHECK(make_dir(usb + "/1-3") && make_dir(usb + "/1-3/1-3:1.0"));
    CHECK(make_dir(usb + "/1-4") && make_dir(usb + "/1-4/1-4:1.0"));
    CHECK(write_file(usb + "/1-4/serial", "C9E2B3F1\n"));
    for (const char* n : {"1", "2", "3"}) CHECK(make_dir(sys + "/card" + n));
    CHECK(::symlink((usb + "/1-4/1-4:1.0").c_str(), (sys + "/card1/device").c_str()) == 0);
    CHECK(::symlink((usb + "/1-3/1-3:1.0").c_str(), (sys + "/card2/device").c_str()) == 0);
    CHECK(::symlink("card1", (proc + "/UMC1820").c_str()) == 0);
    CHECK(::symlink("card2", (proc + "/UMC1820_1").c_str()) == 0);
    CHECK(::symlink("card3", (proc + "/NoDevice").c_str()) == 0);
    const std::string sep(1, '\x01');

    CHECK(alsa_card_unit("UMC1820", proc, sys) == "C9E2B3F1" + sep + "1-4:1.0");
    CHECK(alsa_card_unit("1", proc, sys) == "C9E2B3F1" + sep + "1-4:1.0");
    // No serial: the bus path alone still keeps the units apart.
    CHECK(alsa_card_unit("UMC1820_1", proc, sys) == sep + "1-3:1.0");
    CHECK(alsa_card_unit("NoDevice", proc, sys).empty());
    CHECK(alsa_card_unit("Absent", proc, sys).empty());
    CHECK(alsa_card_unit("", proc, sys).empty());

    for (const char* l : {"/asound/UMC1820", "/asound/UMC1820_1", "/asound/NoDevice",
                          "/sound/card1/device", "/sound/card2/device", "/usb1/1-4/serial"}) {
        std::remove((root + l).c_str());
    }
    for (const char* d : {"/sound/card1", "/sound/card2", "/sound/card3", "/sound", "/asound",
                          "/usb1/1-3/1-3:1.0", "/usb1/1-3", "/usb1/1-4/1-4:1.0", "/usb1/1-4",
                          "/usb1", ""}) {
        ::rmdir((root + d).c_str());
    }
    return 0;
}

// PIPEWIRE_PROPS is how a pw-jack client sets jack.fill-aliases for itself.
// Whatever the operator already put there must survive.
int test_fill_aliases_joins_existing_props() {
    CHECK(with_fill_aliases(nullptr) == "{ jack.fill-aliases = true }");
    CHECK(with_fill_aliases("") == "{ jack.fill-aliases = true }");
    CHECK(with_fill_aliases("{ node.latency = 128/48000 }") ==
          "{ node.latency = 128/48000 jack.fill-aliases = true }");
    CHECK(with_fill_aliases("node.latency = 128/48000") ==
          "node.latency = 128/48000 jack.fill-aliases = true");
    // Already decided by whoever set it: left alone.
    CHECK(with_fill_aliases("{ jack.fill-aliases = false }") ==
          "{ jack.fill-aliases = false }");
    return 0;
}

}  // namespace

int main() {
    if (test_card_from_object_path() != 0) return 1;
    if (test_not_alsa_has_no_card() != 0) return 1;
    if (test_usb_is_what_the_kernel_says() != 0) return 1;
    if (test_unit_is_the_serial_then_the_bus_path() != 0) return 1;
    if (test_fill_aliases_joins_existing_props() != 0) return 1;
    std::printf("test_alsa_card: ok\n");
    return 0;
}
