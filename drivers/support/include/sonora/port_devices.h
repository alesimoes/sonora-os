// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"
#include "sonora/port_order.h"

namespace sonora::drivers {

// Turns full "device:port" names into PortInfo: grouped by device, devices
// in natural order, and channels numbered from 0 within each device. The
// device is everything before the first ':', the JACK client part.
inline std::vector<backend::PortInfo> describe_ports(std::vector<std::string> names) {
    const auto device_of = [](const std::string& n) {
        const std::size_t colon = n.find(':');
        return colon == std::string::npos ? n : n.substr(0, colon);
    };
    std::sort(names.begin(), names.end(),
              [&device_of](const std::string& a, const std::string& b) {
                  const std::string da = device_of(a), db = device_of(b);
                  if (da != db) return natural_less(da, db);
                  return natural_less(a, b);
              });

    std::vector<backend::PortInfo> out;
    out.reserve(names.size());
    uint32_t channel = 0;
    for (std::string& n : names) {
        std::string device = device_of(n);
        if (!out.empty() && out.back().device != device) channel = 0;
        out.push_back({std::move(device), std::move(n), channel++});
    }
    return out;
}

}  // namespace sonora::drivers
