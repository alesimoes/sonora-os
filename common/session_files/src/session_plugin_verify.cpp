// SPDX-License-Identifier: GPL-3.0-or-later
// Plugin files checked at open (session_plugin_verify.h).
#include "sonora/session_files/session_plugin_verify.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "sonora/sha256.h"

namespace sonora::session_files {

namespace {

namespace fs = std::filesystem;

bool hash_file(const fs::path& p, std::string& out) {
    std::FILE* in = std::fopen(p.string().c_str(), "rb");
    if (in == nullptr) return false;
    Sha256 h;
    std::vector<char> buffer(std::size_t{1} << 20);
    std::size_t got = 0;
    while ((got = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) h.update(buffer.data(), got);
    const bool ok = !std::ferror(in);
    std::fclose(in);
    if (ok) out = h.hex();
    return ok;
}

}  // namespace

std::string plugin_file_on_disk_problem(const std::string& folder, const model::PluginFileRef& f) {
    // A received session is untrusted: its path is checked before it is
    // joined to the folder, and every component is lstat'd so a link cannot
    // carry the hash outside it.
    const std::string why = model::plugin_file_problem(f);
    if (!why.empty()) return f.path.substr(0, 256) + ": " + why;
    fs::path at(folder);
    std::error_code ec;
    for (const fs::path& part : fs::path(f.path)) {
        at /= part;
        if (fs::is_symlink(fs::symlink_status(at, ec))) return f.path + " is behind a link";
    }
    const fs::file_status st = fs::symlink_status(at, ec);
    if (ec || !fs::is_regular_file(st)) return f.path + " is missing";
    const std::uintmax_t size = fs::file_size(at, ec);
    if (ec || size != f.bytes) return f.path + " is not the size the session named";
    std::string sha;
    if (!hash_file(at, sha)) return f.path + " could not be read";
    if (sha != f.sha256) return f.path + " does not match its checksum";
    return {};
}

void PluginFileVerifier::open() {
    verified_.clear();
    failed_.clear();
}

std::string PluginFileVerifier::problem(const std::string& folder, const std::vector<model::PluginFileRef>& files) {
    for (const model::PluginFileRef& f : files) {
        const Key key{f.path, f.sha256};
        if (const auto bad = failed_.find(key); bad != failed_.end()) return bad->second;
        if (verified_.count(key)) {
            // Hashed already: a cheap look that it is still there, unlinked
            // and the size it was, without reading it again.
            std::error_code ec;
            fs::path at(folder);
            bool plain = true;
            for (const fs::path& part : fs::path(f.path)) {
                at /= part;
                plain = plain && !fs::is_symlink(fs::symlink_status(at, ec));
            }
            const std::uintmax_t size = fs::file_size(at, ec);
            if (plain && !ec && size == f.bytes) continue;
            verified_.erase(key);
        }
        const std::string why = plugin_file_on_disk_problem(folder, f);
        if (!why.empty()) {
            failed_[key] = why;
            return why;
        }
        verified_.insert(key);
    }
    return {};
}

void PluginFileVerifier::trust(const std::vector<model::PluginFileRef>& files) {
    for (const model::PluginFileRef& f : files)
        if (model::plugin_file_problem(f).empty() && !failed_.count({f.path, f.sha256}))
            verified_.insert({f.path, f.sha256});
}

}  // namespace sonora::session_files
