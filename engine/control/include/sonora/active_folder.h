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
    // removes on leaving if it holds no data (ADR-0018 §3): no take, no
    // session file, only an empty "Audio Files/" and the "Plugin State/"
    // files of flushes no session file names.
    enum class Kind { Named, Untitled };

    // The first free of `root`/Untitled, Untitled 2, Untitled 3.. (the
    // interface's unused_session_name rule), made now, `root` with it if
    // missing. Made, not only found, so two engines never take one name.
    // Empty, with `why`, when none could be made.
    static std::string make_untitled(const std::string& root, std::string& why);
    // Removes each "Untitled" or "Untitled <n>" folder of `root` that holds
    // no data, as above, and that no engine holds: what an engine that died
    // left of a session nothing was recorded or saved in.
    static void remove_empty_untitled(const std::string& root);

    ActiveFolder();
    // Removes the folder if it is Untitled, or a first Save renamed it and
    // then wrote no session file there, and it holds no data.
    ~ActiveFolder();
    ActiveFolder(const ActiveFolder&) = delete;
    ActiveFolder& operator=(const ActiveFolder&) = delete;

    // Makes `path` the session's active folder, in §1's order, on the
    // control thread: refused while a take is open or closing; the folder's
    // lock taken while the old one is still held; "Audio Files/" created,
    // and the folder synced when that made it; unclosed takes recovered,
    // their notices appended to `notices`; the medium read. Then the switch,
    // the old folder removed as the destructor would, and only then the old
    // lock released. The first step that fails returns false with `why` and
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
    // Untitled, and holding nothing but an empty "Audio Files/": a new
    // session may have it as it is.
    bool fresh() const;

    // The first Save of an untitled session (ADR-0018 §3). Whether a Save
    // into `folder` renames the active folder to it: the active folder is
    // Untitled and has never held a session file, `folder` is another one
    // and does not exist, is not inside the active folder, and its parent
    // is on the active folder's filesystem. Otherwise the Save writes where
    // it is told.
    bool names_on_save(const std::string& folder) const;
    // That rename, the lock kept (it is on the folder itself), the session
    // moved to `folder` and the folder Named from then on. Refused while a
    // take is open or closing, naming the folder it is written to.
    bool rename(Session& session, const std::string& folder, std::string& why);

private:
    // Removed on leaving if it holds no data.
    bool disposable() const;

    std::unique_ptr<platform::FolderLock> lock_;
    std::string path_;
    Kind kind_ = Kind::Named;
    bool renamed_ = false;   // named by rename(), not entered by name
    bool read_only_ = false;
    uint64_t free_bytes_ = 0;
};

}  // namespace sonora
