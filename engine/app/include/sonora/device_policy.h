// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's side of the capture device policy (ADR-0007 M2 note, item 8,
// revised by the owner 2026-10-13). select_device() in the contract says
// which ports a name selects; this says what the engine does around its
// startup wait, and what it tells the operator. Control thread only.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"

namespace sonora {

// Where the operator's choice of device is kept, most specific first: the
// operator's home, which is on the persistence partition on the appliance
// (ADR-0005), then `system_dir` (/etc/sonora). --device overrides both.
std::vector<std::string> device_setting_paths(const char* home,
                                              const std::string& system_dir);

// The first line of the first file in `paths` that names a device, trimmed.
// Blank lines and lines starting with '#' name nothing. Empty if none does;
// otherwise `from`, if given, is set to the file it came from.
std::string read_device_setting(const std::vector<std::string>& paths,
                                std::string* from = nullptr);

struct Fallback {
    bool ok = true;
    std::string device;  // what the inputs now come from; empty if nothing
    std::string notice;  // for the screen and stderr; empty unless onboard
    std::string error;   // when !ok
};

// The startup wait is over, no device was named and no input is connected:
// the USB interface if one came at the last moment, else the one non-USB
// device, chosen in the backend (choose_capture_device()) and announced.
// Several non-USB devices are a failure listing them. Nothing at all is
// not: the caller keeps looking. Nor is a backend that already has a device
// (the interface, connected and since unpatched): nothing changes.
Fallback fall_back_after_wait(backend::AudioBackend& audio);

// The notice the screen keeps while recording from a non-USB device that
// was not named.
std::string onboard_notice(const std::string& device);

// Non-empty when `all` has a USB interface, for an engine running on its
// onboard fallback: the inputs are not moved to it mid-session, since that
// would change the channel count under the tracks, so it suggests a
// restart.
std::string late_usb_notice(const std::vector<backend::PortInfo>& all);

// Why no armed track would hear anything, in terms the operator can act on:
// some inputs connected but none to an armed track, the device asked for
// and the ones present, or the several devices to choose between. `inputs`
// is how many the session registered.
std::string why_no_inputs(backend::AudioBackend& audio, const std::string& device,
                          std::size_t inputs);

}  // namespace sonora
