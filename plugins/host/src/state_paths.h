// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LV2 states that hold files (ADR-0029 §6): the state:makePath,
// state:mapPath and state:freePath features one chain instance is given,
// and the folders behind them.
//
//   makePath       resolves under the instance's scratch folder (outside the
//                  session folder, which moves at its first Save and at Save
//                  As), parents created; an absolute path, a ".." or a
//                  symlink out of the folder is refused (NULL).
//   abstract_path  in save(): a file already under the save folder's
//                  "Plugin Files/" (or a previous session folder's) is named
//                  by its path relative to that folder; any other file is
//                  hashed and copied into "<save_dir>/Plugin Files/<sha256>/
//                  <basename>" (tmp, fdatasync, rename) and named by that.
//                  Each is recorded for the State reply. A file over
//                  CopyLimits::file_bytes, a copy that takes the save past
//                  total_bytes, or one that would leave less than
//                  margin_bytes free fails the save before a byte is
//                  written, its text naming the file and the cap; a copy
//                  still running when the save has used CopyLimits::budget_ms
//                  stops and fails the save, so the engine's State deadline
//                  never kills the host half way. A failed copy leaves no
//                  ".incoming-*" behind. "Plugin Files/" and "<sha256>/" are
//                  opened as real directories (O_NOFOLLOW) before anything is
//                  made in them: a symlink there fails the save. A saved file
//                  is read-only (0444). A file under "Plugin Files/" whose
//                  size, mtime or inode is not the one this object last saw
//                  (at restore, or when it saved or hashed it) is hashed again
//                  before its directory's sha names it; one that no longer
//                  matches is copied as new bytes.
//   absolute_path  in restore(): "<session_dir>/<path>"; a path that escapes
//                  the folder, or passes a symlink in it (lstat on every
//                  component), is refused (NULL). At save, a path into a
//                  session folder that resolves outside it fails the save
//                  and is never copied. An absolute path, which
//                  states saved before wire 4 hold, is returned as it is,
//                  and abstract_path names it as given, never copying it in.
//
// At construction (the host's Load) and at the first save into a folder,
// "Plugin Files/.incoming-<pid>-<n>" files whose host pid no longer runs are
// removed: what a host killed mid-copy leaves. A live pid's are another
// host's copy in progress, and stay.
//   free_path      frees what the two above return (malloc'd, so a plugin
//                  that calls free() instead is fine too).
//
// Every one of them allocates and touches the filesystem, so it runs on the
// host's main or worker thread only. A call from the processing thread
// (mark_processing_thread) is refused in constant time: a thread-local flag
// read and an atomic increment, nothing else. Refusals are counted for a
// Fault{Plugin} the main thread sends later (refused()).

#include "sonora/plugin/host_control.h"

#include <lv2/core/lv2.h>
#include <lv2/state/state.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace sonora::plugin_host {

// The processing thread calls this once, before its first block.
void mark_processing_thread() noexcept;
bool on_processing_thread() noexcept;

// What one instance's save may copy (host_control.h kMaxStateFileBytes,
// kMaxStateCopyBytes, kStateCopyFreeMarginBytes). Tests shrink the caps and
// fake the free space; the host uses the defaults.
struct CopyLimits {
    uint64_t file_bytes = plugin::control::kMaxStateFileBytes;
    uint64_t total_bytes = plugin::control::kMaxStateCopyBytes;
    uint64_t margin_bytes = plugin::control::kStateCopyFreeMarginBytes;
    // Bytes free to an unprivileged writer on the filesystem holding `dir`,
    // or false. Empty: statvfs.
    std::function<bool(const std::string& dir, uint64_t& free)> free_space;
    // Milliseconds a save may spend copying and hashing, from begin_save
    // (host_control.h kStateCopyBudgetMs), and the clock that measures it.
    // Empty clock: steady_clock. A test's clock may also act on the file
    // being copied: it is read before every chunk.
    uint64_t budget_ms = plugin::control::kStateCopyBudgetMs;
    std::function<uint64_t()> clock;
};

// Removes "<folder>/Plugin Files/.incoming-<pid>-<n>" whose pid is not
// running, through no link; returns how many.
std::size_t sweep_incoming(const std::string& folder);

class StatePaths {
public:
    // Both absolute or empty (contracts/plugin valid_folder). No scratch
    // folder: no makePath. No session folder: a relative path cannot be
    // resolved, and a save names paths as the plugin gave them.
    StatePaths(std::string scratch_dir, std::string session_dir, CopyLimits limits = {});
    StatePaths(const StatePaths&) = delete;
    StatePaths& operator=(const StatePaths&) = delete;

    // The features, address-stable for this object's life. make_path_feature
    // is null without a scratch folder.
    const LV2_Feature* make_path_feature() const { return scratch_.empty() ? nullptr : &make_f_; }
    const LV2_Feature* map_path_feature() const { return &map_f_; }
    const LV2_Feature* free_path_feature() const { return &free_f_; }

    // Main thread, around one save() (host_control.h GetState). A save_dir
    // also becomes the session folder; the previous one is kept for
    // abstract_path's rewrite. end_save gives the files the state names, or
    // false with the reason the save must fail (a copy that failed, or a
    // file to copy under kGetStateNoCopy).
    void begin_save(const std::string& save_dir, bool no_copy);
    bool end_save(std::vector<plugin::control::StateFile>& files, std::string& error);

    // Any thread: calls refused since the instance was made, and the text
    // naming the last one (main thread).
    uint64_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed) + rt_refused_.load(std::memory_order_relaxed);
    }
    std::string refusal_text() const;

    // The callbacks, public for the C trampolines in state_paths.cpp.
    char* make_path(const char* path);
    char* abstract_path(const char* absolute);
    char* absolute_path(const char* abstract);
    void free_path(char* path);

private:
    bool refuse_rt() noexcept;
    void refuse(std::string text);
    // `canonical` relative to `<folder>/`, when it is a "Plugin Files/
    // <sha256>/<name>" there whose bytes are still that sha: the relative
    // path, sha and size recorded. False with `error` set: the save fails
    // (a check it may not read for under no_copy, or out of time).
    bool name_in(const std::string& canonical, const std::string& folder, plugin::control::StateFile& out,
                 std::string& error);
    bool free_space_of(const std::string& dir, int dir_fd, uint64_t& free) const;
    bool copy_in(const std::string& given, const std::string& canonical, plugin::control::StateFile& out,
                 std::string& error);
    // `given` names a place in the save folder, the session folder or a
    // previous one, and `canonical` (what it resolves to) is not in it: a
    // link in that folder leads out. Under mutex_.
    bool leads_out_of_sessions(const std::string& given, const std::string& canonical) const;
    uint64_t now_ms() const;
    bool out_of_time() const;
    std::string out_of_time_text(const std::string& given) const;
    void sweep_once(const std::string& folder);

    // What a file in "Plugin Files/" was when this object last knew its
    // bytes; a file that is not that any more is hashed again.
    struct Seen {
        uint64_t dev = 0, ino = 0, size = 0;
        int64_t mtime_ns = 0;
        std::string sha;
        bool operator==(const Seen& o) const {
            return dev == o.dev && ino == o.ino && size == o.size && mtime_ns == o.mtime_ns && sha == o.sha;
        }
    };
    static bool seen_now(const std::string& path, Seen& out);

    const std::string scratch_;
    mutable std::mutex mutex_;
    std::string session_;
    std::vector<std::string> previous_;
    // The save in progress.
    bool saving_ = false;
    bool no_copy_ = false;
    std::string save_dir_, failed_;
    std::vector<plugin::control::StateFile> files_;
    std::map<std::string, std::string> named_;  // a path given -> what it was named, this save
    uint64_t copies_ = 0;
    uint64_t copied_bytes_ = 0;  // this save, against limits_.total_bytes
    uint64_t save_started_ms_ = 0;
    std::map<std::string, Seen> seen_;           // by canonical path
    std::set<std::string> passed_through_;       // absolute paths absolute_path returned as given
    std::set<std::string> swept_;                // folders whose stale ".incoming-*" are gone
    const CopyLimits limits_;
    std::string last_refusal_;
    std::atomic<uint64_t> refused_{0}, rt_refused_{0};

    LV2_State_Make_Path make_{};
    LV2_State_Map_Path map_{};
    LV2_State_Free_Path free_{};
    LV2_Feature make_f_{}, map_f_{}, free_f_{};
};

}  // namespace sonora::plugin_host
