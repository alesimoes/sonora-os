// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_paths.h"

#include "sonora/sha256.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace sonora::plugin_host {

namespace fs = std::filesystem;
namespace pc = plugin::control;

namespace {

// Initial-exec TLS in the host executable: reading it neither allocates nor
// locks, whichever plugin library the call comes from.
thread_local bool t_processing = false;

constexpr const char* kPluginFiles = "Plugin Files";

char* dup(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

bool has_dotdot(const std::string& path) {
    for (std::size_t at = 0; at <= path.size();) {
        const std::size_t end = std::min(path.find('/', at), path.size());
        if (path.compare(at, end - at, "..") == 0 && end - at == 2) return true;
        at = end + 1;
    }
    return false;
}

// `path` is `folder` or inside it, both canonical.
bool inside(const std::string& path, const std::string& folder) {
    if (folder.empty() || path.compare(0, folder.size(), folder) != 0) return false;
    return path.size() == folder.size() || folder.back() == '/' || path[folder.size()] == '/';
}

// A symlink stays inside `root` only when it resolves all the way, to a path
// there. weakly_canonical stops at a link whose target is missing, so a
// dangling link out of the folder looked inside, and the plugin's write then
// went through it (found by the #276 review N6 test).
bool link_inside(const fs::path& link, const std::string& root) {
    std::error_code ec;
    const fs::path c = fs::canonical(link, ec);
    return !ec && inside(c.string(), root);
}

std::string canonical_or_empty(const std::string& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return ec ? std::string() : c.string();
}

bool write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= std::size_t(w);
    }
    return true;
}

std::string human_bytes(uint64_t n) {
    char text[32];
    if (n >= (uint64_t{1} << 30)) std::snprintf(text, sizeof text, "%.1f GiB", double(n) / double(uint64_t{1} << 30));
    else if (n >= (uint64_t{1} << 20)) std::snprintf(text, sizeof text, "%.1f MiB", double(n) / double(1u << 20));
    else std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(n));
    return text;
}

// A copy is synced this often, so the dirty pages the last fdatasync has to
// write stay small and the time a copy has taken is the time it really took
// (writes into the page cache alone would look fast until the end).
constexpr uint64_t kSyncEveryBytes = uint64_t{64} << 20;

struct Fd {
    int fd = -1;
    explicit Fd(int f = -1) : fd(f) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    explicit operator bool() const { return fd >= 0; }
};

// `name` under `parent`, as a real directory: made first when absent (mkdirat
// does not follow a link that is already there), then opened without
// following one. A symlink, or anything that is not a folder, is refused, so
// nothing is ever made or written through a link.
int open_real_dir(int parent, const char* name, std::string& why) {
    if (::mkdirat(parent, name, 0755) != 0 && errno != EEXIST) {
        why = std::strerror(errno);
        return -1;
    }
    const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) why = errno == ELOOP || errno == ENOTDIR ? "it is a symlink or not a folder" : std::strerror(errno);
    return fd;
}

// ".incoming-<pid>-<n>": the pid, or 0 for any other name.
pid_t incoming_pid(const char* name) {
    static constexpr char kPrefix[] = ".incoming-";
    if (std::strncmp(name, kPrefix, sizeof kPrefix - 1) != 0) return 0;
    const char* p = name + sizeof kPrefix - 1;
    long long pid = 0;
    const char* digits = p;
    while (*p >= '0' && *p <= '9' && pid < (1LL << 31)) pid = pid * 10 + (*p++ - '0');
    if (p == digits || *p != '-' || pid <= 0 || pid >= (1LL << 31)) return 0;
    const char* n = ++p;
    while (*p >= '0' && *p <= '9') ++p;
    return p != n && *p == '\0' ? pid_t(pid) : 0;
}

char* c_make_path(LV2_State_Make_Path_Handle h, const char* path) {
    return static_cast<StatePaths*>(h)->make_path(path);
}
char* c_abstract_path(LV2_State_Map_Path_Handle h, const char* path) {
    return static_cast<StatePaths*>(h)->abstract_path(path);
}
char* c_absolute_path(LV2_State_Map_Path_Handle h, const char* path) {
    return static_cast<StatePaths*>(h)->absolute_path(path);
}
void c_free_path(LV2_State_Free_Path_Handle h, char* path) { static_cast<StatePaths*>(h)->free_path(path); }

}  // namespace

void mark_processing_thread() noexcept { t_processing = true; }
bool on_processing_thread() noexcept { return t_processing; }

std::size_t sweep_incoming(const std::string& folder) {
    if (folder.empty()) return 0;
    const Fd top(::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!top) return 0;
    const int files = ::openat(top.fd, kPluginFiles, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (files < 0) return 0;  // none yet, or a link: nothing is removed through it
    DIR* dir = ::fdopendir(files);
    if (!dir) {
        ::close(files);
        return 0;
    }
    std::size_t removed = 0;
    const pid_t self = ::getpid();
    while (const dirent* e = ::readdir(dir)) {
        const pid_t pid = incoming_pid(e->d_name);
        // Pid 0 is not ours; a pid that runs is another host's copy in
        // progress (or a recycled pid: the file then waits a session more).
        if (pid == 0 || pid == self || ::kill(pid, 0) == 0 || errno != ESRCH) continue;
        struct stat st {};
        if (::fstatat(files, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
        if (::unlinkat(files, e->d_name, 0) == 0) ++removed;
    }
    ::closedir(dir);
    return removed;
}

StatePaths::StatePaths(std::string scratch_dir, std::string session_dir, CopyLimits limits)
    : scratch_(std::move(scratch_dir)), session_(std::move(session_dir)), limits_(std::move(limits)) {
    make_ = {this, c_make_path};
    map_ = {this, c_abstract_path, c_absolute_path};
    free_ = {this, c_free_path};
    make_f_ = {LV2_STATE__makePath, &make_};
    map_f_ = {LV2_STATE__mapPath, &map_};
    free_f_ = {LV2_STATE__freePath, &free_};
    sweep_once(session_);
}

void StatePaths::sweep_once(const std::string& folder) {
    if (folder.empty() || !swept_.insert(folder).second) return;
    sweep_incoming(folder);
}

uint64_t StatePaths::now_ms() const {
    if (limits_.clock) return limits_.clock();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

bool StatePaths::out_of_time() const { return now_ms() - save_started_ms_ >= limits_.budget_ms; }

std::string StatePaths::out_of_time_text(const std::string& given) const {
    return "its files took more than the " + std::to_string(limits_.budget_ms / 1000) +
           " s a save may spend copying them (stopped at " + given + "): the session disk is too slow for them";
}

bool StatePaths::seen_now(const std::string& path, Seen& out) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out.dev = uint64_t(st.st_dev);
    out.ino = uint64_t(st.st_ino);
    out.size = uint64_t(st.st_size);
    out.mtime_ns = int64_t(st.st_mtim.tv_sec) * 1000000000 + int64_t(st.st_mtim.tv_nsec);
    return true;
}

bool StatePaths::refuse_rt() noexcept {
    if (!t_processing) return false;
    rt_refused_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void StatePaths::refuse(std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_refusal_ = std::move(text);
    refused_.fetch_add(1, std::memory_order_relaxed);
}

std::string StatePaths::refusal_text() const {
    std::string last;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last = last_refusal_;
    }
    const uint64_t rt = rt_refused_.load(std::memory_order_relaxed);
    if (last.empty()) return "a plugin path feature (state:makePath or mapPath) called from the processing thread";
    if (rt) last += "; and " + std::to_string(rt) + " call(s) from the processing thread so far";
    return last;
}

char* StatePaths::make_path(const char* path) {
    if (refuse_rt()) return nullptr;
    const std::string rel = path ? path : "";
    if (scratch_.empty() || rel.empty() || rel[0] == '/' || has_dotdot(rel) ||
        rel.size() > pc::kMaxPathBytes) {
        refuse("makePath refused " + rel.substr(0, 512));
        return nullptr;
    }
    std::error_code ec;
    fs::create_directories(scratch_, ec);
    const std::string root = canonical_or_empty(scratch_);
    if (root.empty()) {
        refuse("makePath: the scratch folder " + scratch_ + " cannot be made");
        return nullptr;
    }
    // Parents one at a time: a symlink already there that leads out of the
    // folder is refused before anything is made through it.
    const fs::path full = fs::path(root) / fs::path(rel).lexically_normal();
    fs::path at(root);
    for (const fs::path& part : fs::path(rel).lexically_normal().parent_path()) {
        at /= part;
        const fs::file_status st = fs::symlink_status(at, ec);
        if (fs::is_symlink(st) && !link_inside(at, root)) {
            refuse("makePath refused " + rel.substr(0, 512) + ": a symlink out of its folder");
            return nullptr;
        }
        if (!fs::exists(at, ec) && !fs::create_directory(at, ec) && ec) {
            refuse("makePath: cannot make " + at.string() + ": " + ec.message());
            return nullptr;
        }
    }
    if (fs::is_symlink(fs::symlink_status(full, ec)) && !link_inside(full, root)) {
        refuse("makePath refused " + rel.substr(0, 512) + ": a symlink out of its folder");
        return nullptr;
    }
    return dup(full.string());
}

bool StatePaths::name_in(const std::string& canonical, const std::string& folder, pc::StateFile& out,
                         std::string& error) {
    const std::string root = canonical_or_empty(folder);
    if (root.empty() || !inside(canonical, root) || canonical.size() <= root.size() + 1) return false;
    const std::string rel = canonical.substr(root.size() + (root.back() == '/' ? 0 : 1));
    const std::string prefix = std::string(kPluginFiles) + "/";
    if (rel.compare(0, prefix.size(), prefix) != 0 || rel.size() < prefix.size() + 64) return false;
    const std::string sha = rel.substr(prefix.size(), 64);
    if (!pc::state_file_path_valid(rel, sha)) return false;
    Seen now;
    // Over the cap (put there by hand): not named by it, so copy_in refuses
    // it with the text, rather than the engine refusing the whole State.
    if (!seen_now(canonical, now) || now.size > limits_.file_bytes) return false;
    now.sha = sha;
    // The directory is the file's hash only while nobody rewrote it: a
    // looper writing its loop back in place, or a received folder whose
    // files do not match their names (#276 review N3). Known unchanged: not
    // read again.
    const auto known = seen_.find(canonical);
    if (known == seen_.end() || !(known->second == now)) {
        if (no_copy_) {
            error = "plugin files are saved after the take (" + rel + " has to be checked first)";
            return false;
        }
        const Fd src(::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!src) return false;
        Sha256 hash;
        std::vector<char> buf(std::size_t(1) << 20);
        uint64_t total = 0;
        for (;;) {
            if (out_of_time()) {
                error = out_of_time_text(rel);
                return false;
            }
            const ssize_t n = ::read(src.fd, buf.data(), buf.size());
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return false;
            if (n == 0) break;
            total += uint64_t(n);
            if (total > now.size) return false;  // growing: it is not the file seen
            hash.update(buf.data(), std::size_t(n));
        }
        Seen after;
        // Not what its directory says, or rewritten while read: new bytes,
        // which copy_in saves under their own hash.
        if (total != now.size || hash.hex() != sha || !seen_now(canonical, after)) return false;
        after.sha = sha;
        if (!(after == now)) return false;
        seen_[canonical] = now;
    }
    out = {rel, sha, now.size};
    return true;
}

bool StatePaths::free_space_of(const std::string& dir, int dir_fd, uint64_t& free) const {
    if (limits_.free_space) return limits_.free_space(dir, free);
    struct statvfs vfs {};
    if (::fstatvfs(dir_fd, &vfs) != 0) return false;
    free = uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
    return true;
}

bool StatePaths::copy_in(const std::string& given, const std::string& canonical, pc::StateFile& out,
                         std::string& error) {
    const fs::path into = fs::path(save_dir_) / kPluginFiles;
    // The save folder is the engine's; what is inside it may be anyone's. A
    // "Plugin Files" or "<sha256>" that is a symlink would send the copy out
    // of the session folder (#276 review N4): each is opened as a real
    // directory, and everything after goes through those descriptors.
    const Fd top(::open(save_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!top) {
        error = "cannot open the session folder " + save_dir_ + ": " + std::strerror(errno);
        return false;
    }
    std::string why;
    const Fd files(open_real_dir(top.fd, kPluginFiles, why));
    if (!files) {
        error = "cannot save plugin files into " + into.string() + ": " + why;
        return false;
    }
    const std::string name = fs::path(given).filename().string();
    // `canonical` has no link left in it; O_NOFOLLOW keeps one swapped in
    // since from being followed, and fstat sizes what was opened.
    const Fd src(::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!src) {
        error = "cannot read " + given + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (::fstat(src.fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "cannot read " + given + ": not a regular file";
        return false;
    }
    // Every refusal before the first byte is written: a plugin's file that
    // would fill the disk or outlast the State budget is never started.
    const uint64_t size = uint64_t(st.st_size);
    if (size > limits_.file_bytes) {
        error = "its file " + given + " is " + human_bytes(size) + ", over the " + human_bytes(limits_.file_bytes) +
                " one plug-in file may be";
        return false;
    }
    if (size > limits_.total_bytes - std::min(copied_bytes_, limits_.total_bytes)) {
        error = "its files come to more than the " + human_bytes(limits_.total_bytes) + " one save may copy (" +
                given + " is " + human_bytes(size) + ", after " + human_bytes(copied_bytes_) + ")";
        return false;
    }
    uint64_t free_bytes = 0;
    if (!free_space_of(into.string(), files.fd, free_bytes)) {
        error = "cannot tell how much room is left in " + into.string() + ": " + std::strerror(errno);
        return false;
    }
    if (free_bytes < size || free_bytes - size < limits_.margin_bytes) {
        error = "not enough room to save " + given + " (" + human_bytes(size) + "): " + human_bytes(free_bytes) +
                " free in " + into.string() + ", and " + human_bytes(limits_.margin_bytes) +
                " must stay free for recording";
        return false;
    }
    // The pid names the host that owns it, so a host killed mid-copy leaves
    // a file the next session open can tell is nobody's (sweep_incoming).
    const std::string tmp = ".incoming-" + std::to_string(::getpid()) + "-" + std::to_string(++copies_);
    // Read-only from its creation: a saved plugin file is never written again
    // (#276 review N3); this descriptor is the one writer it has.
    const int dst = ::openat(files.fd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0444);
    if (dst < 0) {
        error = "cannot write " + (into / tmp).string() + ": " + std::strerror(errno);
        return false;
    }
    // Hashed while copied: one read of the plugin's file, never held whole.
    Sha256 sha;
    std::vector<char> buf(std::size_t(1) << 20);
    uint64_t total = 0, unsynced = 0;
    bool ok = true;
    for (;;) {
        // Stopped with a text while the engine still waits, rather than
        // killed by its deadline with the copy half done.
        if (out_of_time()) {
            error = out_of_time_text(given);
            ok = false;
            break;
        }
        const ssize_t n = ::read(src.fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            error = "cannot read " + given + ": " + std::strerror(errno);
            ok = false;
            break;
        }
        if (n == 0) break;
        total += uint64_t(n);
        // The plugin can still be writing it: what was checked is the most
        // that is copied.
        if (total > size) {
            error = "its file " + given + " grew while it was being saved";
            ok = false;
            break;
        }
        sha.update(buf.data(), std::size_t(n));
        if (!write_all(dst, buf.data(), std::size_t(n))) {
            error = "cannot write " + (into / tmp).string() + ": " + std::strerror(errno);
            ok = false;
            break;
        }
        unsynced += uint64_t(n);
        if (unsynced >= kSyncEveryBytes) {
            if (::fdatasync(dst) != 0) {
                error = "cannot sync " + (into / tmp).string() + ": " + std::strerror(errno);
                ok = false;
                break;
            }
            unsynced = 0;
        }
    }
    if (ok && ::fdatasync(dst) != 0) {
        error = "cannot sync " + (into / tmp).string() + ": " + std::strerror(errno);
        ok = false;
    }
    struct stat written {};
    if (ok && ::fstat(dst, &written) != 0) {
        error = "cannot read back " + (into / tmp).string() + ": " + std::strerror(errno);
        ok = false;
    }
    ::close(dst);
    const std::string hex = sha.hex();
    const std::string rel = ok ? pc::plugin_file_path(hex, name) : std::string();
    if (ok && rel.empty()) {
        error = "a plugin file whose name cannot be kept: " + given;
        ok = false;
    }
    if (ok) {
        const Fd dir(open_real_dir(files.fd, hex.c_str(), why));
        // Moved over whatever has the name: the same bytes when a state or an
        // earlier save put them there, and otherwise a file that no longer
        // matches its hash, which these bytes put right. A link there is
        // replaced, not followed.
        if (!dir) {
            error = "cannot save plugin files into " + (into / hex).string() + ": " + why;
            ok = false;
        } else if (::renameat(files.fd, tmp.c_str(), dir.fd, name.c_str()) != 0) {
            error = "cannot move a plugin file into " + (into / hex / name).string() + ": " + std::strerror(errno);
            ok = false;
        }
    }
    if (!ok) {
        ::unlinkat(files.fd, tmp.c_str(), 0);
        return false;
    }
    copied_bytes_ += total;
    Seen saved;
    saved.dev = uint64_t(written.st_dev);
    saved.ino = uint64_t(written.st_ino);
    saved.size = uint64_t(written.st_size);
    saved.mtime_ns = int64_t(written.st_mtim.tv_sec) * 1000000000 + int64_t(written.st_mtim.tv_nsec);
    saved.sha = hex;
    seen_[canonical_or_empty((fs::path(save_dir_) / rel).string())] = saved;
    out = {rel, hex, total};
    return true;
}

char* StatePaths::abstract_path(const char* absolute) {
    if (refuse_rt()) return nullptr;
    const std::string given = absolute ? absolute : "";
    std::lock_guard<std::mutex> lock(mutex_);
    // Relative already (a path this host gave), or nowhere to save: as given,
    // which is wire 3's behaviour.
    if (given.empty() || given[0] != '/' || save_dir_.empty()) return dup(given);
    if (const auto it = named_.find(given); it != named_.end()) return dup(it->second);
    const std::string canonical = canonical_or_empty(given);
    pc::StateFile f;
    std::string check_error;
    bool named = !canonical.empty() && name_in(canonical, save_dir_, f, check_error);
    // A file restored from the folder this session had before its first
    // Save or a Save As: the same relative path, which the engine's Save As
    // brings along (ADR-0029 §6).
    for (std::size_t k = previous_.size(); !named && check_error.empty() && !canonical.empty() && k-- > 0;)
        named = name_in(canonical, previous_[k], f, check_error);
    if (!check_error.empty()) {
        if (failed_.empty()) failed_ = check_error;
        return dup(given);
    }
    if (!named) {
        std::error_code ec;
        if (canonical.empty() || !fs::is_regular_file(canonical, ec)) {
            // Missing, or a folder: named as given, as before wire 4; the
            // plugin decides what a missing file means at restore.
            return dup(given);
        }
        // An absolute path a restored state held (ADR-0029 §6): it came from
        // session data, which may come from anyone, so it is named as it was
        // and never copied in. Otherwise a received session naming
        // ~/.ssh/id_rsa would carry that file away in its own Plugin Files/
        // at the next save (#276 review N5).
        if (passed_through_.count(given) || passed_through_.count(canonical)) return dup(given);
        if (no_copy_) {
            if (failed_.empty()) failed_ = "plugin files are saved after the take (" + given + ")";
            return dup(given);
        }
        // The State already fails: copying more would only spend the budget.
        if (!failed_.empty()) return dup(given);
        std::string error;
        if (!copy_in(given, canonical, f, error)) {
            if (failed_.empty()) failed_ = error;
            return dup(given);
        }
    }
    bool listed = false;
    for (const pc::StateFile& have : files_) listed = listed || have.path == f.path;
    if (!listed) {
        if (files_.size() >= pc::kMaxStateFiles) {
            if (failed_.empty())
                failed_ = "the state names more than " + std::to_string(pc::kMaxStateFiles) + " files";
            return dup(given);
        }
        files_.push_back(f);
    }
    named_[given] = f.path;
    return dup(f.path);
}

char* StatePaths::absolute_path(const char* abstract) {
    if (refuse_rt()) return nullptr;
    const std::string given = abstract ? abstract : "";
    // States saved before wire 4 (lilv's own mapPath, no folders) hold
    // absolute paths; they stay what they were, and are remembered so a save
    // does not copy them in (abstract_path).
    if (!given.empty() && given[0] == '/') {
        if (given.size() <= pc::kMaxPathBytes) {
            const std::string canonical = canonical_or_empty(given);
            std::lock_guard<std::mutex> lock(mutex_);
            passed_through_.insert(given);
            if (!canonical.empty()) passed_through_.insert(canonical);
        }
        return dup(given);
    }
    std::string session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_;
    }
    if (given.empty() || has_dotdot(given) || session.empty() || given.size() > pc::kMaxPathBytes) {
        refuse("absolute_path refused " + given.substr(0, 512) +
               (session.empty() ? ": the session has no folder" : ": it escapes the session folder"));
        return nullptr;
    }
    const std::string full = (fs::path(session) / given).string();
    // What the session's file is as the plugin gets it: the next save names
    // it by its directory's sha without reading it, unless it changed.
    Seen seen;
    const std::string canonical = canonical_or_empty(full);
    const std::string prefix = std::string(kPluginFiles) + "/";
    if (!canonical.empty() && given.compare(0, prefix.size(), prefix) == 0 && given.size() > prefix.size() + 64 &&
        pc::state_file_path_valid(given, given.substr(prefix.size(), 64)) && seen_now(canonical, seen)) {
        seen.sha = given.substr(prefix.size(), 64);
        std::lock_guard<std::mutex> lock(mutex_);
        seen_[canonical] = seen;
    }
    return dup(full);
}

void StatePaths::free_path(char* path) {
    // free() on the processing thread may take the allocator's lock: leaked.
    if (refuse_rt()) return;
    std::free(path);
}

void StatePaths::begin_save(const std::string& save_dir, bool no_copy) {
    std::lock_guard<std::mutex> lock(mutex_);
    saving_ = true;
    no_copy_ = no_copy;
    save_dir_ = save_dir;
    failed_.clear();
    files_.clear();
    named_.clear();
    copied_bytes_ = 0;
    // Within a reply's transit of when the engine's deadline for this GetState
    // starts (sent, or the previous State on this host answered): the 15 s
    // kStateCopyBudgetMs leaves over cover the difference.
    save_started_ms_ = now_ms();
    sweep_once(save_dir);
    if (!save_dir.empty() && save_dir != session_) {
        if (!session_.empty()) {
            bool known = false;
            for (const std::string& p : previous_) known = known || p == session_;
            if (!known) previous_.push_back(session_);
        }
        session_ = save_dir;
    }
}

bool StatePaths::end_save(std::vector<pc::StateFile>& files, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    saving_ = false;
    save_dir_.clear();
    if (!failed_.empty()) {
        error = failed_;
        files.clear();
        return false;
    }
    files = std::move(files_);
    files_.clear();
    return true;
}

}  // namespace sonora::plugin_host
