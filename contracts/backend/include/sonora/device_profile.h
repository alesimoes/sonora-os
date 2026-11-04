// SPDX-License-Identifier: GPL-3.0-or-later
//
// Contract file (ADR-0007 §4.2). Human names for the channels of interfaces
// we know, so the input and output pickers say "ADAT 3" and not
// "capture_AUX12". The port names themselves are never changed: natural_less
// and the capture device policy depend on capture_AUX<n>, and a label is
// display only.
//
// Header only and backend-neutral: a backend passes PortInfo::device (the
// description the server gives the card, which for a USB card is the product
// string from the descriptor) and the channel number within that device.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sonora::backend {

enum class Direction { capture, playback };

// True when `device` names a Behringer UMC1820. Case-insensitive, so it
// matches "UMC1820 Pro", "Behringer UMC1820" and "USB Audio UMC1820".
inline bool is_umc1820(const std::string& device) {
    static const char key[] = "umc1820";
    const std::size_t n = sizeof(key) - 1;
    if (device.size() < n) return false;
    for (std::size_t i = 0; i + n <= device.size(); ++i) {
        std::size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(device[i + k])) == key[k]) ++k;
        if (k == n) return true;
    }
    return false;
}

// The label of channel `channel` (0-based, in natural_less order) of a known
// device, or "" when the device is unknown or the channel is past its range;
// callers then show the number. UMC1820 order, from the manual as quoted in
// brain/research/umc1820-profile.md (second-hand until measured on hardware):
//   capture : 1-8 mic/line, 9-10 S/PDIF, 11-18 ADAT
//   playback: 1-10 analog, 11-12 S/PDIF, 13-20 ADAT
// With nothing on the ADAT input the interface presents 10 in / 12 out, and
// the first ten / twelve labels still hold.
inline std::string channel_label(const std::string& device, Direction dir, uint32_t channel) {
    if (!is_umc1820(device)) return {};
    const auto numbered = [](const char* prefix, uint32_t n) {
        return std::string(prefix) + " " + std::to_string(n);
    };
    const auto side = [](const char* prefix, uint32_t i) {
        return std::string(prefix) + (i == 0 ? " L" : " R");
    };
    if (dir == Direction::capture) {
        if (channel < 8) return numbered("Mic/Line", channel + 1);
        if (channel < 10) return side("S/PDIF", channel - 8);
        if (channel < 18) return numbered("ADAT", channel - 9);
    } else {
        if (channel < 10) return numbered("Out", channel + 1);
        if (channel < 12) return side("S/PDIF", channel - 10);
        if (channel < 20) return numbered("ADAT", channel - 11);
    }
    return {};
}

}  // namespace sonora::backend
