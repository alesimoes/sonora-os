// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"
#include "sonora/port_order.h"

namespace sonora::drivers {

namespace port_devices_detail {

inline std::string client_of(const std::string& n) {
    const std::size_t colon = n.find(':');
    return colon == std::string::npos ? n : n.substr(0, colon);
}

// The "-<digits>" pw-jack 1.4.2 appends to a port whose name another node
// of the same description already holds, or npos.
inline std::size_t suffix_at(const std::string& n) {
    const std::size_t dash = n.rfind('-');
    if (dash == std::string::npos || dash + 1 == n.size()) return std::string::npos;
    if (n.find(':') > dash) return std::string::npos;
    for (std::size_t i = dash + 1; i < n.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(n[i]))) return std::string::npos;
    }
    return dash;
}

}  // namespace port_devices_detail

// Turns full "device:port" names into PortInfo: grouped by device, devices
// in natural order, and channels numbered from 0 within each device. `usb`
// says, by port name, whether the port is on a USB device; without it none
// is.
//
// The JACK client part is the node's description, and two units of one
// model share it: pw-jack keeps one client name and gives the second unit's
// ports a "-<global id>" suffix, an id handed out in creation order. Taken
// as one device, natural_less would interleave the two units' channels. So
// a client is split into one device per hardware unit, by `unit`: per port,
// a key that names the unit the same way on every boot (its serial, then
// its bus path), empty when the backend cannot tell. Units are ordered by
// key and named "<client> #1", "#2", ...; a client with one unit keeps its
// bare name. Ports with no key fall back to the suffix: the bare names are
// one unit and each suffix that shadows a bare name another. That keeps the
// units apart but not their order, which follows creation order then.
inline std::vector<backend::PortInfo> describe_ports(
    std::vector<std::string> names,
    const std::function<bool(const std::string&)>& usb = nullptr,
    const std::function<std::string(const std::string&)>& unit = nullptr) {
    using port_devices_detail::client_of;
    using port_devices_detail::suffix_at;

    struct Entry {
        std::string name;
        std::string client;
        std::string key;   // the unit's stable key, or empty
        std::string mark;  // with no key: the pw-jack suffix, or empty
        uint32_t rank = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(names.size());
    for (std::string& n : names) {
        Entry e;
        e.client = client_of(n);
        if (unit) e.key = unit(n);
        e.name = std::move(n);
        entries.push_back(std::move(e));
    }

    // A suffix marks a unit only if it shadows a bare name in the same
    // client: a port may end in "-2" of its own accord.
    for (Entry& e : entries) {
        if (!e.key.empty()) continue;
        const std::size_t at = suffix_at(e.name);
        if (at == std::string::npos) continue;
        const std::string bare = e.name.substr(0, at);
        const std::string suffix = e.name.substr(at);
        for (const Entry& other : entries) {
            if (other.key.empty() && other.name == bare) {
                e.mark = suffix;
                break;
            }
        }
    }
    for (Entry& e : entries) {
        if (!e.key.empty() || e.mark.empty()) continue;
        // The whole unit carries the mark, including any port whose bare
        // twin is missing.
        for (Entry& other : entries) {
            if (other.key.empty() && other.mark.empty() && other.client == e.client &&
                suffix_at(other.name) != std::string::npos &&
                other.name.substr(suffix_at(other.name)) == e.mark) {
                other.mark = e.mark;
            }
        }
    }

    // Units of a client, in order: keyed ones by key, then the bare names,
    // then each suffix by its number.
    const auto unit_less = [](const Entry& a, const Entry& b) {
        if (a.key.empty() != b.key.empty()) return !a.key.empty();
        if (!a.key.empty()) return natural_less(a.key, b.key);
        return natural_less(a.mark, b.mark);
    };
    const auto same_unit = [](const Entry& a, const Entry& b) {
        return a.client == b.client && a.key == b.key && a.mark == b.mark;
    };
    std::vector<uint32_t> units_in(entries.size(), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        uint32_t rank = 0, units = 0;
        std::vector<const Entry*> seen;
        for (const Entry& other : entries) {
            if (other.client != entries[i].client) continue;
            bool counted = false;
            for (const Entry* s : seen) counted = counted || same_unit(*s, other);
            if (counted) continue;
            seen.push_back(&other);
            ++units;
            if (unit_less(other, entries[i])) ++rank;
        }
        entries[i].rank = rank;
        units_in[i] = units;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (units_in[i] > 1) {
            entries[i].client += " #" + std::to_string(entries[i].rank + 1);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.client != b.client) return natural_less(a.client, b.client);
        return natural_less(a.name, b.name);
    });

    std::vector<backend::PortInfo> out;
    out.reserve(entries.size());
    uint32_t channel = 0;
    for (Entry& e : entries) {
        if (!out.empty() && out.back().device != e.client) channel = 0;
        const bool on_usb = usb && usb(e.name);
        out.push_back({std::move(e.client), std::move(e.name), channel++, on_usb});
    }
    return out;
}

}  // namespace sonora::drivers
