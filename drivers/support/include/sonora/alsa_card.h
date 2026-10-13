// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <string>

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
