// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linked instead of alsa_system.cpp where libasound's headers are absent, so
// the backend and its tests still build; AlsaBackend then refuses to open and
// says why.
#include "sonora/alsa_device.h"

namespace sonora::alsa {

std::unique_ptr<System> make_system() { return nullptr; }

}  // namespace sonora::alsa
