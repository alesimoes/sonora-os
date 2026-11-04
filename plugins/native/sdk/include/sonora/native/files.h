// SPDX-License-Identifier: GPL-3.0-or-later
//
// File slots (ADR-0029 §6, "Native plugins' file slots"): a processor whose
// data is a file, such as the cabinet's impulse response, declares slots
// here. A slot is state, not a parameter: it is saved with the plugin's
// state as a path, and the host decides where that path lives (the session's
// Plugin Files folder, through LV2 state:mapPath). The processor only reads
// the path it is given.
//
// Everything on this interface is for a control thread. load() opens a file
// and allocates, so a format adapter calls it from the thread where a host
// restores state (LV2 restore(), VST3 setState()), never from process().
// The processor makes the new data audible without a lock or an allocation
// on the audio thread, as the cabinet does with its kernel hand-over.
#pragma once

#include <cstdint>
#include <string>

namespace sonora::native {

struct FileSlotInfo {
    const char* id;          // stable and append-only, like a parameter id: the state key
    const char* name;        // shown by a file chooser
    const char* extensions;  // lower-case, comma separated, no dots: "wav"
};

class FileSlots {
public:
    virtual ~FileSlots() = default;

    virtual std::uint32_t slot_count() const noexcept = 0;
    virtual const FileSlotInfo& slot(std::uint32_t index) const noexcept = 0;

    // Reads `path` into the slot. An empty path clears the slot to the
    // processor's default. On failure the slot keeps what it had and `error`
    // says why.
    virtual bool load(std::uint32_t index, const std::string& path, std::string& error) = 0;
    // The path the slot last loaded, empty for the default. Control threads.
    virtual std::string path(std::uint32_t index) const = 0;
};

}  // namespace sonora::native
