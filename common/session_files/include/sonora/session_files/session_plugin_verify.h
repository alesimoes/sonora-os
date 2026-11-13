// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin files a session's states name, checked against their recorded
// digests (ADR-0151 §2): "Plugin Files/<sha256>/<basename>" must hash to
// <sha256>. Run when a session opens, on the control thread, so a damaged
// file is found before a take rather than when a plugin loads it mid-session.
// Never on the audio thread: it reads whole files.
#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::session_files {

// Empty when `f` is a plain file under `folder` (no link on the way), of the
// size and digest it names; else "<path> <why>", naming the file.
std::string plugin_file_on_disk_problem(const std::string& folder, const model::PluginFileRef& f);

// Hashes each file once per opened session: a chain started again later
// (a restart, an edit, during a take) checks only what it has not verified,
// so no multi-gigabyte hash runs while recording.
class PluginFileVerifier {
public:
    // A session opened: nothing of the last one is trusted.
    void open();
    // Empty when every one of `files` verifies under `folder` (the session's
    // folder now: Save As carries the files, and what was verified with
    // them); else the first problem, by name. A file that failed once keeps
    // failing, unhashed, until the next open.
    std::string problem(const std::string& folder, const std::vector<model::PluginFileRef>& files);
    // Files the host hashed as it wrote them and the saver made durable.
    // One that failed this session stays failed: a save that copied nothing
    // (kGetStateNoCopy) names the file already there without reading it.
    void trust(const std::vector<model::PluginFileRef>& files);

private:
    using Key = std::pair<std::string, std::string>;  // (path, sha256)
    std::set<Key> verified_;
    std::map<Key, std::string> failed_;
};

}  // namespace sonora::session_files
