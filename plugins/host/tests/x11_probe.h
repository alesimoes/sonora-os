// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the editor scenarios of plugin_driver see of the host's windows, as
// the operator would: through the X server, never through the host. Its own
// translation unit, so Xlib's macros (None, Bool, Status) stay out of the
// driver.

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace x11_probe {

struct Toplevel {
    uint64_t id = 0;
    std::string name, res_class;
    int width = 0, height = 0;
    bool viewable = false;
    bool fixed_size = false;  // min = max size hints
};

// The toplevels whose _NET_WM_PID is `pid`: how many, and the first.
int find(pid_t pid, Toplevel& first);
// WM_DELETE_WINDOW, as a window manager's close button sends it.
bool request_close(uint64_t id);

}  // namespace x11_probe
