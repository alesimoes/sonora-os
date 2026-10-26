// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/active_folder.h"

#include <filesystem>
#include <system_error>

#include "sonora/closed_take.h"
#include "sonora/platform/file.h"
#include "sonora/session.h"

namespace sonora {

namespace {

namespace fs = std::filesystem;

// What two spellings of one folder compare equal as: "a/../b", "./b" and a
// symlink to b are all b.
std::string identity(const std::string& path) {
    std::error_code absolute_ec;
    const fs::path absolute = fs::absolute(path, absolute_ec);
    if (absolute_ec) return path;
    std::error_code canonical_ec;
    const fs::path p = fs::weakly_canonical(absolute, canonical_ec);
    return canonical_ec ? absolute.string() : p.string();
}

// True if it is a folder now; `made` says whether this call made it.
bool make_folder(const std::string& path, bool& made, std::string& why) {
    std::error_code ec;
    made = fs::create_directory(path, ec);
    if (!ec && fs::is_directory(path, ec)) return true;
    why = "cannot create " + path + (ec ? ": " + ec.message() : ": not a folder");
    return false;
}

}  // namespace

ActiveFolder::ActiveFolder() = default;
ActiveFolder::~ActiveFolder() = default;

bool ActiveFolder::enter(Session& session, const std::string& path, Kind kind,
                          std::string& why, std::vector<std::string>& notices) {
    why = session.folder_change_refusal();
    if (!why.empty()) {
        why = "the session folder cannot change while " + why;
        return false;
    }

    const std::string id = identity(path);
    const bool same = lock_ != nullptr && id == path_;
    // The lock is taken on the folder itself, so it has to exist first.
    // Making an empty folder is all a refusal below can leave behind.
    bool made_folder = false;
    if (!make_folder(path, made_folder, why)) return false;
    // Make before break: the old lock is held until the switch, so a
    // refusal here leaves the old folder exactly as it was.
    std::unique_ptr<platform::FolderLock> lock;
    if (!same) {
        lock = std::make_unique<platform::FolderLock>();
        std::string err;
        switch (lock->acquire(path, err)) {
            case platform::FolderLock::Result::Held:
                break;
            case platform::FolderLock::Result::Busy:
                why = err + "; one session takes one engine. Stop the other engine first.";
                return false;
            case platform::FolderLock::Result::Failed:
                why = err;
                return false;
        }
    }

    bool made_audio = false;
    if (!make_folder(path + "/Audio Files", made_audio, why)) return false;
    // Best effort: what a power cut could lose here is an empty folder, and
    // the unsupported platform claims no durability at all.
    if (made_audio) platform::sync_directory(path);
    if (made_folder) {
        const fs::path parent = fs::path(id).parent_path();
        if (!parent.empty()) platform::sync_directory(parent.string());
    }

    // Before anything can record there: a take an earlier engine died
    // recording gets the length that reached the disk.
    for (std::string& line : recover_unclosed_takes(path, session.config().sample_rate)) {
        notices.push_back(std::move(line));
    }

    const bool read_only = platform::read_only(path);
    uint64_t bytes = 0;
    if (!platform::free_bytes(path, bytes)) bytes = 0;

    if (!session.set_folder(path, why)) return false;
    read_only_ = read_only;
    free_bytes_ = bytes;
    if (same) {
        kind_ = kind;
        return true;
    }
    const std::string old_path = path_;
    const Kind old_kind = kind_;
    lock_ = std::move(lock);
    path_ = id;
    kind_ = kind;
    if (old_kind == Kind::Untitled && !old_path.empty()) {
        // remove() is rmdir for a folder, which fails on one with anything
        // in it: this can never delete a file.
        std::error_code ec;
        fs::remove(fs::path(old_path) / "Audio Files", ec);
        fs::remove(fs::path(old_path), ec);
    }
    return true;
}

}  // namespace sonora
