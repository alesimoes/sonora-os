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
//                  written, its text naming the file and the cap; a failed
//                  copy leaves no ".incoming-*" behind.
//   absolute_path  in restore(): "<session_dir>/<path>"; a path that escapes
//                  the folder is refused (NULL). An absolute path, which
//                  states saved before wire 4 hold, is returned as it is.
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
};

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
    // <sha256>/<name>" there: the relative path, sha and size recorded.
    bool name_in(const std::string& canonical, const std::string& folder, plugin::control::StateFile& out) const;
    bool free_space_of(const std::string& dir, uint64_t& free) const;
    bool copy_in(const std::string& given, const std::string& canonical, plugin::control::StateFile& out,
                 std::string& error);

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
    const CopyLimits limits_;
    std::string last_refusal_;
    std::atomic<uint64_t> refused_{0}, rt_refused_{0};

    LV2_State_Make_Path make_{};
    LV2_State_Map_Path map_{};
    LV2_State_Free_Path free_{};
    LV2_Feature make_f_{}, map_f_{}, free_f_{};
};

}  // namespace sonora::plugin_host
