// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entering a session folder (ADR-0018 §1): the steps around
// Session::set_folder() that engine/core cannot take itself, because the
// lock is common/platform's and recovery is this module's (ADR-0007 §1).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora {

class Session;
namespace platform {
class FolderLock;
}

class ActiveFolder {
public:
    // Untitled: an "Untitled <n>" folder the engine made itself, which it
    // removes on leaving if nothing was put in it (ADR-0018 §3).
    enum class Kind { Named, Untitled };

    ActiveFolder();
    ~ActiveFolder();
    ActiveFolder(const ActiveFolder&) = delete;
    ActiveFolder& operator=(const ActiveFolder&) = delete;

    // Makes `path` the session's active folder, in §1's order, on the
    // control thread: refused while a take is open or closing; the folder's
    // lock taken while the old one is still held; "Audio Files/" created,
    // and the folder synced when that made it; unclosed takes recovered,
    // their notices appended to `notices`; the medium read. Then the switch,
    // the old lock released, and the old folder removed if it was Untitled
    // and empty. The first step that fails returns false with `why` and
    // leaves the old folder active and locked. Entering the folder already
    // active keeps its lock and runs the rest again.
    bool enter(Session& session, const std::string& path, Kind kind, std::string& why,
               std::vector<std::string>& notices);

    // What the medium said when the active folder was entered (§8).
    // Recording is not yet refused on it: that is checklist item 7.
    bool read_only() const { return read_only_; }
    // 0 when it could not be read.
    uint64_t free_bytes() const { return free_bytes_; }
    bool untitled() const { return kind_ == Kind::Untitled; }

private:
    std::unique_ptr<platform::FolderLock> lock_;
    std::string path_;
    Kind kind_ = Kind::Named;
    bool read_only_ = false;
    uint64_t free_bytes_ = 0;
};

}  // namespace sonora
