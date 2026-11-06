// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <stdlib.h>
#include <unistd.h>

namespace sonora::drivers {

// The ALSA card id in a PipeWire object.path: "alsa:acp:<card>:..." from
// the ACP device (the Pro Audio profile included) or "alsa:pcm:<card>:..."
// with ACP off (spa/plugins/alsa, PipeWire 1.4.2). Empty for anything else,
// and for an id that is not a plain kernel card id, since it becomes a path.
inline std::string alsa_card_of(const std::string& object_path) {
    std::string rest;
    for (const char* prefix : {"alsa:acp:", "alsa:pcm:"}) {
        if (object_path.rfind(prefix, 0) == 0) {
            rest = object_path.substr(std::string(prefix).size());
            break;
        }
    }
    const std::string card = rest.substr(0, rest.find(':'));
    for (const char c : card) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            return {};
        }
    }
    return card;
}

// Whether the kernel's snd-usb-audio owns the card: it, and only it, gives
// a card /proc/asound/cardN/usbid. The kernel also links /proc/asound/<id>
// to cardN, so an id works as well as an index.
inline bool alsa_card_is_usb(const std::string& card,
                             const std::string& proc_asound = "/proc/asound") {
    if (card.empty()) return false;
    bool index = true;
    for (const char c : card) index = index && std::isdigit(static_cast<unsigned char>(c));
    const std::string dir = proc_asound + "/" + (index ? "card" + card : card);
    return ::access((dir + "/usbid").c_str(), F_OK) == 0;
}

// The hardware unit behind an ALSA card, as a key that is the same on every
// boot: "<serial>\x01<bus path>". The card id is not that key: two units of
// one model get "UMC1820" and "UMC1820_1" in probe order. The serial comes
// first so a unit keeps its place when its cable moves to another port; the
// bus path (the sysfs name, "1-3:1.0" for a USB interface) keeps two units
// apart when they report no serial or the same one, and orders them by the
// port they are plugged into. Empty when the card or its sysfs device
// cannot be found.
inline std::string alsa_card_unit(const std::string& card,
                                  const std::string& proc_asound = "/proc/asound",
                                  const std::string& sys_class_sound = "/sys/class/sound") {
    if (card.empty()) return {};
    bool index = true;
    for (const char c : card) index = index && std::isdigit(static_cast<unsigned char>(c));
    std::string number = card;
    if (!index) {
        // The kernel links /proc/asound/<id> to "cardN".
        char link[64] = {};
        const ssize_t n = ::readlink((proc_asound + "/" + card).c_str(), link, sizeof(link) - 1);
        if (n <= 4 || std::string(link, 4) != "card") return {};
        number = std::string(link + 4, static_cast<std::size_t>(n) - 4);
    }
    char* resolved = ::realpath((sys_class_sound + "/card" + number + "/device").c_str(), nullptr);
    if (!resolved) return {};
    const std::string device = resolved;
    std::free(resolved);
    const std::size_t slash = device.rfind('/');
    const std::string path = slash == std::string::npos ? device : device.substr(slash + 1);
    const std::string parent = slash == std::string::npos ? std::string() : device.substr(0, slash);
    // A USB serial is on the device, the card on one of its interfaces.
    std::string serial;
    for (const std::string& dir : {device, parent}) {
        std::FILE* f = std::fopen((dir + "/serial").c_str(), "r");
        if (!f) continue;
        char line[256] = {};
        if (std::fgets(line, sizeof(line), f)) serial = line;
        std::fclose(f);
        while (!serial.empty() && std::isspace(static_cast<unsigned char>(serial.back()))) {
            serial.pop_back();
        }
        break;
    }
    return serial + '\x01' + path;
}

// PIPEWIRE_PROPS with jack.fill-aliases turned on. pw-jack 1.4.2 leaves a
// port's aliases empty unless the client asks, and alias 1 is the
// object.path the bus is read from. A value that already says anything
// about fill-aliases is the operator's decision and is kept as it is.
inline std::string with_fill_aliases(const char* existing) {
    const std::string kSetting = "jack.fill-aliases = true";
    std::string props = existing ? existing : "";
    if (props.find("jack.fill-aliases") != std::string::npos) return props;
    const std::size_t last = props.find_last_not_of(" \t\n");
    if (last == std::string::npos) return "{ " + kSetting + " }";
    if (props[last] == '}') {
        std::size_t end = props.find_last_not_of(" \t\n", last - 1);
        end = end == std::string::npos ? 0 : end + 1;
        return props.substr(0, end) + " " + kSetting + " " + props.substr(last);
    }
    return props + " " + kSetting;
}

}  // namespace sonora::drivers
