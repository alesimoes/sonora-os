// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/active_folder.h"

#include <filesystem>
#include <iterator>
#include <system_error>

#include "sonora/closed_take.h"
#include "sonora/platform/file.h"
#include "sonora/session.h"
#include "sonora/session_files/session_folder.h"

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

const char kUntitled[] = "Untitled";

// "Untitled", or "Untitled <n>": the names make_untitled() gives.
bool untitled_name(const std::string& name) {
    const std::string base = kUntitled;
    if (name == base) return true;
    if (name.size() <= base.size() + 1 || name.compare(0, base.size() + 1, base + " ") != 0) {
        return false;
    }
    for (std::size_t i = base.size() + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

// A folder and not a symlink to one: whatever a symlink points to is the
// operator's, and nothing here may follow it.
bool real_folder(const fs::directory_entry& e) {
    std::error_code ec;
    return fs::is_directory(e.symlink_status(ec)) && !ec;
}

bool real_folder(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec)) && !ec;
}

// Nothing in `dir` but an empty "Audio Files/", which entering made.
bool holds_nothing(const std::string& dir) {
    if (!real_folder(fs::path(dir))) return false;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() != "Audio Files") return false;
        std::error_code inner;
        if (!real_folder(*it) || !fs::is_empty(it->path(), inner) || inner) return false;
    }
    return !ec;
}

bool holds_a_session_file(const std::string& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".sonora") return true;
    }
    return false;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Only plugin state files, or nothing: what flushes leave there. Each one
// checked is appended to `states`, if given.
bool holds_only_states(const fs::path& dir, std::vector<fs::path>* states) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code inner;
        if (!fs::is_regular_file(it->symlink_status(inner)) || inner ||
            !(ends_with(name, ".state") || ends_with(name, ".state.tmp"))) {
            return false;
        }
        if (states != nullptr) states->push_back(it->path());
    }
    return !ec;
}

// No take, no session file, nothing the operator put there: an empty
// "Audio Files/" and at most the "Plugin State/" files flushes wrote. No
// session file names those states and no take needs them, so they are not
// the operator's data (ADR-0018 §3). A symlink anywhere is data. The state
// files it checked are appended to `states`, if given.
bool holds_no_data(const std::string& dir, std::vector<fs::path>* states = nullptr) {
    if (!real_folder(fs::path(dir))) return false;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code inner;
        if (!real_folder(*it)) return false;
        if (name == "Audio Files") {
            if (!fs::is_empty(it->path(), inner) || inner) return false;
        } else if (name != session_files::kPluginStateDir || !holds_only_states(it->path(), states)) {
            return false;
        }
    }
    return !ec;
}

// Only the state files holds_no_data() checked are unlinked: a file that
// arrived after the check is not one it vouched for. The folders are
// rmdir'd, which fails on anything else in them. The caller holds the
// folder's lock, so no other engine can enter it meanwhile.
void remove_unreferenced(const std::string& dir) {
    std::vector<fs::path> files;
    if (!holds_no_data(dir, &files)) return;
    std::error_code ec;
    for (const fs::path& f : files) fs::remove(f, ec);
    fs::remove(fs::path(dir) / session_files::kPluginStateDir, ec);
    fs::remove(fs::path(dir) / "Audio Files", ec);
    fs::remove(fs::path(dir), ec);
}

// Whether `p` is `dir` or lies inside it, both in identity()'s spelling.
bool within(const fs::path& p, const fs::path& dir) {
    auto pi = p.begin();
    for (auto di = dir.begin(); di != dir.end(); ++di, ++pi) {
        if (di->empty() && std::next(di) == dir.end()) break;   // a trailing '/'
        if (pi == p.end() || *pi != *di) return false;
    }
    return true;
}

}  // namespace

std::string ActiveFolder::make_untitled(const std::string& root, std::string& why) {
    std::error_code ec;
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) {
        why = "cannot make the sessions folder " + root + (ec ? ": " + ec.message() : ": not a folder");
        return {};
    }
    // Bounded only so that a filesystem that answers nonsense cannot spin
    // the control thread for ever.
    for (int n = 1; n < 100000; ++n) {
        const std::string name = n == 1 ? std::string(kUntitled) : kUntitled + (" " + std::to_string(n));
        const fs::path p = fs::path(root) / name;
        std::error_code made_ec;
        if (fs::create_directory(p, made_ec)) {
            platform::sync_directory(root);
            return p.string();
        }
        if (made_ec) {
            why = "cannot make " + p.string() + ": " + made_ec.message();
            return {};
        }
    }
    why = "every Untitled name in " + root + " is taken";
    return {};
}

void ActiveFolder::remove_empty_untitled(const std::string& root) {
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!real_folder(*it) || !untitled_name(it->path().filename().string())) continue;
        const std::string dir = it->path().string();
        if (!holds_no_data(dir)) continue;
        // Held, it is another engine's new session, empty because nothing
        // was recorded yet.
        platform::FolderLock lock;
        std::string err;
        if (lock.acquire(dir, err) != platform::FolderLock::Result::Held) continue;
        remove_unreferenced(dir);
    }
}

ActiveFolder::ActiveFolder() = default;

ActiveFolder::~ActiveFolder() {
    // While the lock is still held, so no other engine has entered it.
    if (disposable()) remove_unreferenced(path_);
}

bool ActiveFolder::disposable() const {
    // A first Save that renamed the folder and then failed to write its
    // file leaves a named folder with no session file in it.
    return (kind_ == Kind::Untitled || renamed_) && !path_.empty();
}

bool ActiveFolder::fresh() const {
    return kind_ == Kind::Untitled && !path_.empty() && holds_nothing(path_);
}

bool ActiveFolder::names_on_save(const std::string& folder) const {
    if (kind_ != Kind::Untitled || path_.empty() || holds_a_session_file(path_)) return false;
    const std::string id = identity(folder);
    // Inside it, rename(2) would refuse with EINVAL; the Save writes there
    // instead, as told.
    if (within(fs::path(id), fs::path(path_))) return false;
    std::error_code ec;
    if (fs::exists(id, ec) || ec) return false;
    const fs::path parent = fs::path(id).parent_path();
    return fs::is_directory(parent, ec) && platform::same_filesystem(parent.string(), path_);
}

bool ActiveFolder::rename(Session& session, const std::string& folder, std::string& why) {
    const std::string busy = session.folder_change_refusal();
    if (!busy.empty()) {
        why = "stop recording to name the session; the take is being written to " +
              fs::path(path_).filename().string() + " (" + busy + ")";
        return false;
    }
    if (!names_on_save(folder)) {
        why = "the session's folder cannot be renamed to " + folder;
        return false;
    }
    const std::string id = identity(folder);
    std::string caution;
    if (!platform::rename_dir_durably(path_, id, caution)) {
        why = caution;
        return false;
    }
    if (!session.set_folder(folder, why)) {
        // Unreachable while the refusal above holds on this thread; put
        // back rather than leave the session naming a folder that is gone.
        std::string back;
        platform::rename_dir_durably(id, path_, back);
        return false;
    }
    path_ = id;
    kind_ = Kind::Named;
    renamed_ = true;
    why = caution;
    return true;
}

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
    // A Save As killed mid-copy (ADR-0018 §5, step 4): no file names a ".part".
    session_files::sweep_partial_copies(path);

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
    const bool old_disposable = disposable();
    // Released only once the old folder is gone, as the destructor does:
    // released first, another engine could enter it between the check and
    // the unlinks.
    const std::unique_ptr<platform::FolderLock> old_lock = std::move(lock_);
    lock_ = std::move(lock);
    path_ = id;
    kind_ = kind;
    renamed_ = false;
    if (old_disposable) remove_unreferenced(old_path);
    return true;
}

}  // namespace sonora
