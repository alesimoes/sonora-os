// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The editors of one chain, as HostControl's OpenEditor and CloseEditor see
// them (contracts/plugin host_control.h, "Editors"). Main thread only. The
// display is opened at the first OpenEditor, so a host whose editors are
// never opened never connects to a display, and one with no display answers
// EditorFailed and plays on.

#include "chain.h"
#include "editor.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin_host {

class Editors {
public:
    explicit Editors(Chain& chain) : chain_(chain), windows_(chain.size()) {}
    ~Editors() { close_all(); }
    Editors(const Editors&) = delete;
    Editors& operator=(const Editors&) = delete;

    // The editor shown, or false with the reason (EditorFailed's text). One
    // already open is raised and refused: EditorClosed comes once for each
    // EditorOpened, so a second one cannot be said.
    bool open(uint32_t instance, const std::string& parent_token, std::string& error);
    // True when it was open (and now is not).
    bool close(uint32_t instance);
    bool is_open(uint32_t instance) const { return instance < windows_.size() && windows_[instance] != nullptr; }
    std::vector<uint32_t> open_instances() const;
    void close_all();

    // Window events, the plugins' descriptors and timers. The instances
    // whose window the operator closed are closed here and appended to
    // `closed`. Returns when the next timer is due (monotonic_ns), 0 none.
    uint64_t dispatch(std::vector<uint32_t>& closed);

private:
    Chain& chain_;
    std::unique_ptr<EditorDisplay> display_;
    std::vector<std::unique_ptr<EditorWindow>> windows_;
};

}  // namespace sonora::plugin_host
