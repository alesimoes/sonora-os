// SPDX-License-Identifier: GPL-3.0-or-later
//
// What /proc/asound says about a USB card, and how --device finds a card.
// Both are pure so they are tested on text and lists. /proc is read because
// opening a PCM to count its channels fails with EBUSY while PipeWire holds
// the card, and "the interface is absent" would then be a lie.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "sonora/alsa_device.h"

namespace sonora::alsa {

struct StreamSummary {
    uint32_t capture_channels = 0;
    uint32_t playback_channels = 0;
    // Every discrete rate any altsetting lists. Empty when none was listed
    // or any line was a continuous range (then the stream negotiates).
    std::vector<uint32_t> rates;
};

// /proc/asound/cardN/stream0 of snd-usb-audio: "Playback:" and "Capture:"
// sections, each with one block per altsetting carrying "Channels: N" and
// "Rates: ...". The largest channel count of a direction is what the card
// can be opened with. The format of the file is the kernel's, read from
// sound/usb/proc.c; the UMC1820's own file is still to be captured.
inline StreamSummary parse_stream0(const std::string& text) {
    StreamSummary s;
    bool capture = false, playback = false, continuous = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const std::string t = line.substr(first);
        if (first == 0) {
            capture = t.rfind("Capture:", 0) == 0;
            playback = t.rfind("Playback:", 0) == 0;
            continue;
        }
        if (t.rfind("Channels:", 0) == 0) {
            const auto n = static_cast<uint32_t>(std::strtoul(t.c_str() + 9, nullptr, 10));
            if (capture) s.capture_channels = std::max(s.capture_channels, n);
            if (playback) s.playback_channels = std::max(s.playback_channels, n);
        } else if (t.rfind("Rates:", 0) == 0 && (capture || playback)) {
            if (t.find("continuous") != std::string::npos || t.find(" - ") != std::string::npos) {
                continuous = true;
                continue;
            }
            std::istringstream rates(t.substr(6));
            std::string item;
            while (std::getline(rates, item, ',')) {
                const auto r = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10));
                if (r != 0 && std::find(s.rates.begin(), s.rates.end(), r) == s.rates.end()) {
                    s.rates.push_back(r);
                }
            }
        }
    }
    if (continuous) s.rates.clear();
    std::sort(s.rates.begin(), s.rates.end());
    return s;
}

namespace detail {
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace detail

// The card `wanted` names, as its id: the id, the name or the index,
// optionally written hw:ID, hw:CARD=ID or hw:N,0. Case does not matter in
// names. Empty when none matches; the caller says which cards there are.
inline std::string resolve_card(const std::vector<CardInfo>& cards, const std::string& wanted) {
    std::string w = wanted;
    if (w.rfind("hw:", 0) == 0 || w.rfind("plughw:", 0) == 0) {
        w = w.substr(w.find(':') + 1);
        if (w.rfind("CARD=", 0) == 0) w = w.substr(5);
        w = w.substr(0, w.find(','));
    }
    const std::string lw = detail::lower(w);
    for (const CardInfo& c : cards) {
        if (detail::lower(c.id) == lw || detail::lower(c.name) == lw ||
            std::to_string(c.index) == w) {
            return c.id;
        }
    }
    return {};
}

}  // namespace sonora::alsa
