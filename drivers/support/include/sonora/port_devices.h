// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sonora/audio_backend.h"
#include "sonora/port_order.h"

namespace sonora::drivers {

// Splits "<base>-<digits>" when the dash lies at or after `from`: true, with
// `base` and `id` set. The digits are pipewire-jack's global id.
inline bool split_id_suffix(const std::string& s, std::size_t from, std::string& base,
                            unsigned long long& id) {
    const std::size_t dash = s.rfind('-');
    if (dash == std::string::npos || dash < from || dash + 1 == s.size() ||
        s.size() - dash - 1 > 18) {
        return false;
    }
    for (std::size_t i = dash + 1; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    base = s.substr(0, dash);
    id = std::stoull(s.substr(dash + 1));
    return true;
}

// The unit a port belongs to when several devices share one
// node.description, as two interfaces of one model do. pipewire-jack names
// a JACK client after node.description and makes the clash unique with
// "-<global id>", in one of two places (measured on 1.4.2,
// brain/research/multichannel-usb-interfaces.md):
//  - nodes the session manager creates, as ALSA nodes are: one client, and
//    each later port that collides gets the suffix ("Dup Pro:capture_AUX1"
//    and "Dup Pro:capture_AUX1-49");
//  - separate JACK clients: the later client's name gets it ("Dup
//    Pro:capture_AUX0" and "Dup Pro-46:capture_AUX0").
// A suffix counts only when the bare name exists too, so a name that ends
// in "-<n>" of its own is left alone. `base` is the bare client name, and
// `unit` 0 for the first unit, else 1, 2, ... in id order among the copies
// of the same name: ids are handed out in creation order, and a node's
// ports are created together. Port copies rank before client copies.
struct PortUnit {
    std::string base;
    uint32_t unit = 0;
};

inline std::vector<PortUnit> port_units(const std::vector<std::string>& names) {
    const auto client_of = [](const std::string& n) { return n.substr(0, n.find(':')); };
    const std::set<std::string> all(names.begin(), names.end());
    std::set<std::string> clients;
    for (const std::string& n : names) clients.insert(client_of(n));

    // bare name -> ids of its suffixed copies, sorted below
    std::map<std::string, std::vector<unsigned long long>> port_copies, client_copies;
    struct Copy {
        std::string base;
        unsigned long long id = 0;
        bool client = false;
    };
    std::vector<Copy> copy(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        const std::size_t colon = n.find(':');
        std::string base;
        unsigned long long id = 0;
        if (colon != std::string::npos && split_id_suffix(n, colon, base, id) &&
            all.count(base) != 0) {
            port_copies[base].push_back(id);
            copy[i] = {std::move(base), id, false};
            continue;
        }
        const std::string client = client_of(n);
        if (split_id_suffix(client, 0, base, id) && clients.count(base) != 0) {
            std::vector<unsigned long long>& ids = client_copies[base];
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            copy[i] = {std::move(base), id, true};
        }
    }
    for (auto& c : port_copies) std::sort(c.second.begin(), c.second.end());
    for (auto& c : client_copies) std::sort(c.second.begin(), c.second.end());
    const auto rank = [](const std::vector<unsigned long long>& ids, unsigned long long id) {
        return static_cast<uint32_t>(std::lower_bound(ids.begin(), ids.end(), id) - ids.begin() +
                                     1);
    };
    std::vector<PortUnit> out(names.size());
    std::map<std::string, uint32_t> port_units_of;  // bare client -> most units from port copies
    for (std::size_t i = 0; i < names.size(); ++i) {
        const Copy& c = copy[i];
        if (c.base.empty()) {
            out[i].base = client_of(names[i]);
        } else if (!c.client) {
            out[i].base = client_of(c.base);
            out[i].unit = rank(port_copies[c.base], c.id);
            uint32_t& most = port_units_of[out[i].base];
            most = std::max(most, out[i].unit);
        }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const Copy& c = copy[i];
        if (!c.client) continue;
        out[i].base = c.base;
        out[i].unit = port_units_of[c.base] + rank(client_copies[c.base], c.id);
    }
    return out;
}

// Renumbers `units` so that, within each client, a unit's number follows its
// key and not the server's creation order: ports with one key are one unit
// whatever suffix they came with, keyed units rank by natural order of the
// key, and ports with no key keep their suffix units after the keyed ones.
// A client left with one unit keeps number 0, hence its bare name.
inline void rank_units_by_key(const std::vector<std::string>& names,
                              const std::function<std::string(const std::string&)>& unit,
                              std::vector<PortUnit>& units) {
    struct Group {
        std::string key;    // empty for a keyless unit
        uint32_t suffix = 0;  // port_units' number, for the keyless ones
    };
    const auto group_less = [](const Group& a, const Group& b) {
        if (a.key.empty() != b.key.empty()) return !a.key.empty();
        if (!a.key.empty()) return natural_less(a.key, b.key);
        return a.suffix < b.suffix;
    };
    const auto same_group = [](const Group& a, const Group& b) {
        return a.key == b.key && (!a.key.empty() || a.suffix == b.suffix);
    };
    std::map<std::string, std::vector<Group>> groups_of;  // client base -> its units
    std::vector<Group> group(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        group[i] = {unit(names[i]), units[i].unit};
        std::vector<Group>& groups = groups_of[units[i].base];
        bool seen = false;
        for (const Group& g : groups) seen = seen || same_group(g, group[i]);
        if (!seen) groups.push_back(group[i]);
    }
    for (auto& [base, groups] : groups_of) std::sort(groups.begin(), groups.end(), group_less);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::vector<Group>& groups = groups_of[units[i].base];
        for (std::size_t k = 0; k < groups.size(); ++k) {
            if (same_group(groups[k], group[i])) units[i].unit = static_cast<uint32_t>(k);
        }
    }
}

// Turns full "device:port" names into PortInfo: grouped by device, devices
// in natural order, and channels numbered from 0 within each device. The
// device is everything before the first ':', the JACK client part, except
// that a second unit of the same model (see port_units) is its own device,
// "<client> (2)", "(3)", ..., whichever of pipewire-jack's two suffixes it
// came with: left as one, its channels would interleave with the first
// unit's and a track would record the other unit's input with nothing
// looking wrong. `usb` says, by port name, whether the port is on a USB
// device; without it none is.
//
// Which physical unit is "(2)" follows the order the server created them
// in, so it would change from boot to boot, and the device name the operator
// saved (--device, the device setting file) would then name the other unit.
// `unit` fixes that: per port, a key that names its hardware unit the same
// way on every boot (alsa_card_unit(): serial, then bus path), or empty when
// the backend cannot tell. Within a client the keyed units are ranked by
// key, and the units with no key after them in suffix order, so a unit keeps
// its name whichever of them the server enumerated first. The port names
// themselves are left as the server gave them.
inline std::vector<backend::PortInfo> describe_ports(
    std::vector<std::string> names,
    const std::function<bool(const std::string&)>& usb = nullptr,
    const std::function<std::string(const std::string&)>& unit = nullptr) {
    std::vector<PortUnit> units = port_units(names);
    if (unit) rank_units_by_key(names, unit, units);
    std::vector<std::pair<std::string, std::string>> named;  // device, port
    named.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string device = units[i].base;
        if (units[i].unit > 0) device += " (" + std::to_string(units[i].unit + 1) + ")";
        named.emplace_back(std::move(device), std::move(names[i]));
    }
    std::sort(named.begin(), named.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) {
                  if (a.first != b.first) return natural_less(a.first, b.first);
                  return natural_less(a.second, b.second);
              });

    std::vector<backend::PortInfo> out;
    out.reserve(named.size());
    uint32_t channel = 0;
    for (auto& [device, n] : named) {
        if (!out.empty() && out.back().device != device) channel = 0;
        const bool on_usb = usb && usb(n);
        out.push_back({std::move(device), std::move(n), channel++, on_usb});
    }
    return out;
}

}  // namespace sonora::drivers
