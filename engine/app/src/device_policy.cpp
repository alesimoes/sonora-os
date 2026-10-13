// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/device_policy.h"

#include <fstream>

namespace sonora {

using backend::PortInfo;

std::vector<std::string> device_setting_paths(const char* home,
                                              const std::string& system_dir) {
    std::vector<std::string> paths;
    if (home != nullptr && home[0] != '\0') {
        paths.push_back(std::string(home) + "/.config/sonora/device");
    }
    paths.push_back(system_dir + "/device");
    return paths;
}

std::string read_device_setting(const std::vector<std::string>& paths,
                                std::string* from) {
    constexpr const char* kSpace = " \t\r\n";
    for (const std::string& path : paths) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t first = line.find_first_not_of(kSpace);
            if (first == std::string::npos || line[first] == '#') continue;
            const std::size_t last = line.find_last_not_of(kSpace);
            if (from != nullptr) *from = path;
            return line.substr(first, last - first + 1);
        }
    }
    return {};
}

Fallback fall_back_after_wait(backend::AudioBackend& audio) {
    Fallback f;
    std::vector<PortInfo> chosen;
    if (!backend::select_device(audio.physical_capture_ports(), "", chosen, f.error,
                                true)) {
        f.ok = false;
        return f;
    }
    if (chosen.empty()) return f;
    if (chosen.front().usb) {
        f.device = chosen.front().device;
        return f;
    }
    // Refused when the backend already has a device: the inputs stay where
    // they are, and announcing onboard would be false.
    if (!audio.choose_capture_device(chosen.front().device)) return f;
    f.device = chosen.front().device;
    f.notice = onboard_notice(f.device);
    return f;
}

std::string onboard_notice(const std::string& device) {
    return "recording from " + device + " (onboard): no USB interface found";
}

std::string late_usb_notice(const std::vector<PortInfo>& all) {
    for (const PortInfo& p : all) {
        if (!p.usb) continue;
        return "USB interface " + p.device +
               " found; restart the engine to record from it (not switched"
               " mid-session: the channels would change)";
    }
    return {};
}

std::string why_no_inputs(backend::AudioBackend& audio, const std::string& device,
                          std::size_t inputs) {
    const int connected = audio.connected_input_count();
    if (connected > 0) {
        return std::to_string(connected) + " of " + std::to_string(inputs) +
               " inputs are connected, none of them to an armed track";
    }
    const std::vector<PortInfo> all = audio.physical_capture_ports();
    std::vector<PortInfo> chosen;
    std::string why;
    if (!backend::select_device(all, device, chosen, why)) return why;
    if (chosen.empty()) return backend::describe_absent(all, device);
    return "nothing is connected to the inputs, though " + chosen.front().device +
           " is present";
}

}  // namespace sonora
