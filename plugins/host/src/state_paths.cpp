// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_paths.h"

#include "sonora/sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
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

StatePaths::StatePaths(std::string scratch_dir, std::string session_dir, CopyLimits limits)
    : scratch_(std::move(scratch_dir)), session_(std::move(session_dir)), limits_(std::move(limits)) {
    make_ = {this, c_make_path};
    map_ = {this, c_abstract_path, c_absolute_path};
    free_ = {this, c_free_path};
    make_f_ = {LV2_STATE__makePath, &make_};
    map_f_ = {LV2_STATE__mapPath, &map_};
    free_f_ = {LV2_STATE__freePath, &free_};
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
        if (fs::is_symlink(st) && !inside(canonical_or_empty(at.string()), root)) {
            refuse("makePath refused " + rel.substr(0, 512) + ": a symlink out of its folder");
            return nullptr;
        }
        if (!fs::exists(at, ec) && !fs::create_directory(at, ec) && ec) {
            refuse("makePath: cannot make " + at.string() + ": " + ec.message());
            return nullptr;
        }
    }
    if (fs::is_symlink(fs::symlink_status(full, ec)) && !inside(canonical_or_empty(full.string()), root)) {
        refuse("makePath refused " + rel.substr(0, 512) + ": a symlink out of its folder");
        return nullptr;
    }
    return dup(full.string());
}

bool StatePaths::name_in(const std::string& canonical, const std::string& folder, pc::StateFile& out) const {
    const std::string root = canonical_or_empty(folder);
    if (root.empty() || !inside(canonical, root) || canonical.size() <= root.size() + 1) return false;
    const std::string rel = canonical.substr(root.size() + (root.back() == '/' ? 0 : 1));
    const std::string prefix = std::string(kPluginFiles) + "/";
    if (rel.compare(0, prefix.size(), prefix) != 0 || rel.size() < prefix.size() + 64) return false;
    const std::string sha = rel.substr(prefix.size(), 64);
    if (!pc::state_file_path_valid(rel, sha)) return false;
    std::error_code ec;
    const uintmax_t size = fs::file_size(canonical, ec);
    // Over the cap (put there by hand): not named by it, so copy_in refuses
    // it with the text, rather than the engine refusing the whole State.
    if (ec || uint64_t(size) > limits_.file_bytes) return false;
    // Content-addressed and append-only: the directory is its hash, so it is
    // not read again (with kGetStateNoCopy nothing may be).
    out = {rel, sha, uint64_t(size)};
    return true;
}

bool StatePaths::free_space_of(const std::string& dir, uint64_t& free) const {
    if (limits_.free_space) return limits_.free_space(dir, free);
    struct statvfs vfs {};
    if (::statvfs(dir.c_str(), &vfs) != 0) return false;
    free = uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
    return true;
}

bool StatePaths::copy_in(const std::string& given, const std::string& canonical, pc::StateFile& out,
                         std::string& error) {
    std::error_code ec;
    const fs::path into = fs::path(save_dir_) / kPluginFiles;
    fs::create_directories(into, ec);
    if (ec) {
        error = "cannot make " + into.string() + ": " + ec.message();
        return false;
    }
    const std::string name = fs::path(given).filename().string();
    // `canonical` has no link left in it; O_NOFOLLOW keeps one swapped in
    // since from being followed, and fstat sizes what was opened.
    const int src = ::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (src < 0) {
        error = "cannot read " + given + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (::fstat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "cannot read " + given + ": not a regular file";
        ::close(src);
        return false;
    }
    // Every refusal before the first byte is written: a plugin's file that
    // would fill the disk or outlast the State budget is never started.
    const uint64_t size = uint64_t(st.st_size);
    if (size > limits_.file_bytes) {
        error = "its file " + given + " is " + human_bytes(size) + ", over the " + human_bytes(limits_.file_bytes) +
                " one plug-in file may be";
        ::close(src);
        return false;
    }
    if (size > limits_.total_bytes - std::min(copied_bytes_, limits_.total_bytes)) {
        error = "its files come to more than the " + human_bytes(limits_.total_bytes) + " one save may copy (" +
                given + " is " + human_bytes(size) + ", after " + human_bytes(copied_bytes_) + ")";
        ::close(src);
        return false;
    }
    uint64_t free_bytes = 0;
    if (!free_space_of(into.string(), free_bytes)) {
        error = "cannot tell how much room is left in " + into.string() + ": " + std::strerror(errno);
        ::close(src);
        return false;
    }
    if (free_bytes < size || free_bytes - size < limits_.margin_bytes) {
        error = "not enough room to save " + given + " (" + human_bytes(size) + "): " + human_bytes(free_bytes) +
                " free in " + into.string() + ", and " + human_bytes(limits_.margin_bytes) +
                " must stay free for recording";
        ::close(src);
        return false;
    }
    const fs::path tmp = into / (".incoming-" + std::to_string(::getpid()) + "-" + std::to_string(++copies_));
    const int dst = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (dst < 0) {
        error = "cannot write " + tmp.string() + ": " + std::strerror(errno);
        ::close(src);
        return false;
    }
    // Hashed while copied: one read of the plugin's file, never held whole.
    Sha256 sha;
    std::vector<char> buf(std::size_t(1) << 20);
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(src, buf.data(), buf.size());
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
            error = "cannot write " + tmp.string() + ": " + std::strerror(errno);
            ok = false;
            break;
        }
    }
    ::close(src);
    if (ok && ::fdatasync(dst) != 0) {
        error = "cannot sync " + tmp.string() + ": " + std::strerror(errno);
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
        const fs::path final_path = fs::path(save_dir_) / rel;
        fs::create_directories(final_path.parent_path(), ec);
        if (fs::exists(final_path, ec) && fs::file_size(final_path, ec) == total) {
            // Already saved, by another state or an earlier save: the same bytes.
            fs::remove(tmp, ec);
        } else if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
            error = "cannot move a plugin file into " + final_path.string() + ": " + std::strerror(errno);
            ok = false;
        }
    }
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    copied_bytes_ += total;
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
    bool named = !canonical.empty() && name_in(canonical, save_dir_, f);
    // A file restored from the folder this session had before its first
    // Save or a Save As: the same relative path, which the engine's Save As
    // brings along (ADR-0029 §6).
    for (std::size_t k = previous_.size(); !named && !canonical.empty() && k-- > 0;)
        named = name_in(canonical, previous_[k], f);
    if (!named) {
        std::error_code ec;
        if (canonical.empty() || !fs::is_regular_file(canonical, ec)) {
            // Missing, or a folder: named as given, as before wire 4; the
            // plugin decides what a missing file means at restore.
            return dup(given);
        }
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
    // absolute paths; they stay what they were.
    if (!given.empty() && given[0] == '/') return dup(given);
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
    return dup((fs::path(session) / given).string());
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
