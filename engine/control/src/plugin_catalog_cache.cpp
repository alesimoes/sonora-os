// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin catalog cache (plugin_catalog_cache.h). The scan worker's
// thread only: files, stats and fsync.
#include "sonora/plugin_catalog_cache.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#include "sonora/json/json.h"
#include "sonora/platform/file.h"
#include "sonora/platform/process.h"

namespace sonora {

namespace fs = std::filesystem;

namespace {

void account(BundleFingerprint& fp, const platform::PathStatus& s) {
    fp.newest_ns = std::max({fp.newest_ns, s.modified_ns, s.changed_ns});
    if (s.regular) {
        ++fp.files;
        fp.bytes += s.size;
    }
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += ch;
    }
    return out + "\"";
}

bool get_u64(const json::Value* v, uint64_t& out) {
    int64_t n = 0;
    if (!v || !v->is_number() || !v->to_int64(n) || n < 0) return false;
    out = uint64_t(n);
    return true;
}

}  // namespace

BundleFingerprint fingerprint_bundle(const std::string& path) {
    BundleFingerprint fp;
    platform::PathStatus root;
    if (!platform::path_status(path, root)) {
        fp.complete = false;
        return fp;
    }
    account(fp, root);
    if (!root.directory) return fp;
    std::error_code ec;
    // Symlinks followed (change B): yabridge's ~/.vst3/yabridge links into
    // the Wine prefix, and what changes is the target. A link loop ends at
    // the entry cap, as an incomplete walk.
    fs::recursive_directory_iterator it(
        path, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
    uint32_t entries = 0;
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (++entries > kMaxFingerprintEntries) {
            fp.complete = false;
            return fp;
        }
        platform::PathStatus s;
        // A dangling link counts as an entry with nothing behind it: a
        // target that comes back changes the count.
        if (platform::path_status(it->path().string(), s)) account(fp, s);
    }
    if (ec) fp.complete = false;
    return fp;
}

std::string encode_plugin_cache(const PluginCatalogCache& cache) {
    std::string out = "{\"version\": " + std::to_string(kPluginCacheVersion) + ",\n \"host\": " +
                      quoted(cache.host) + ",\n \"build\": " + quoted(cache.build) + ",\n \"bundles\": {";
    bool first = true;
    for (const auto& [path, b] : cache.bundles) {
        // The reader is strict UTF-8 (common/json): a path or a plugin's
        // text that is not would make the whole file unreadable at the next
        // boot. Left out instead, it is only probed again.
        if (!json::valid_utf8(path)) continue;
        out += first ? "\n  " : ",\n  ";
        first = false;
        out += quoted(path) + ": {\"format\": " + quoted(b.format) + ", \"fingerprint\": {\"files\": " +
               std::to_string(b.fingerprint.files) + ", \"bytes\": " + std::to_string(b.fingerprint.bytes) +
               ", \"newest_ns\": " + std::to_string(b.fingerprint.newest_ns) +
               ", \"complete\": " + (b.fingerprint.complete ? "true" : "false") + "}";
        if (!b.failed.empty() && json::valid_utf8(b.failed)) out += ", \"failed\": " + quoted(b.failed);
        out += ", \"plugins\": [";
        bool p_first = true;
        for (const plugin::probe::Record& r : b.plugins) {
            const std::string line = plugin::probe::encode(r);
            if (!json::valid_utf8(line)) continue;
            out += p_first ? "\n    " : ",\n    ";
            p_first = false;
            out += line;
        }
        out += "]}";
    }
    out += "\n }\n}\n";
    return out;
}

CacheLoad load_plugin_cache(const std::string& path, const std::string& host, const std::string& build,
                            PluginCatalogCache& out, std::string& why) {
    out = PluginCatalogCache{};
    out.host = host;
    out.build = build;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        if (!fs::exists(path, ec)) return CacheLoad::Missing;
        why = "the plug-in catalog cache " + path + " cannot be read";
        return CacheLoad::Discarded;
    }
    if (size > kMaxPluginCacheBytes) {
        why = "the plug-in catalog cache " + path + " is " + std::to_string(size) + " bytes, over the " +
              std::to_string(kMaxPluginCacheBytes >> 20) + " MiB cap";
        return CacheLoad::Discarded;
    }
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!f) {
            why = "the plug-in catalog cache " + path + " cannot be read";
            return CacheLoad::Discarded;
        }
        text = ss.str();
    }
    json::Value doc;
    json::Error je;
    json::Limits limits;
    limits.max_input_bytes = kMaxPluginCacheBytes;
    if (!json::parse(text, doc, je, limits) || !doc.is_object()) {
        why = "the plug-in catalog cache " + path + " is not readable JSON" +
              (je.message.empty() ? "" : " (" + je.message + ", line " + std::to_string(je.line) + ")");
        return CacheLoad::Discarded;
    }
    uint64_t version = 0;
    if (!get_u64(doc.find("version"), version) || version != kPluginCacheVersion) {
        why = "the plug-in catalog cache is of another version";
        return CacheLoad::Discarded;
    }
    const json::Value* h = doc.find("host");
    const json::Value* b = doc.find("build");
    if (!h || !h->is_string() || h->text() != host) {
        why = "the plug-in catalog cache was written by another plug-in host (" +
              (h && h->is_string() ? h->text() : std::string("none")) + ")";
        return CacheLoad::Discarded;
    }
    if (!b || !b->is_string() || b->text() != build) {
        why = "the plug-in catalog cache was written by another image build";
        return CacheLoad::Discarded;
    }
    const json::Value* bundles = doc.find("bundles");
    if (!bundles || !bundles->is_object()) {
        why = "the plug-in catalog cache has no bundles";
        return CacheLoad::Discarded;
    }
    for (const auto& [bpath, v] : bundles->members()) {
        const json::Value* format = v.find("format");
        const json::Value* fpv = v.find("fingerprint");
        const json::Value* plugins = v.find("plugins");
        if (!v.is_object() || !format || !format->is_string() || !fpv || !fpv->is_object() || !plugins ||
            !plugins->is_array()) {
            why = "the plug-in catalog cache's entry for " + bpath + " is malformed";
            out.bundles.clear();
            return CacheLoad::Discarded;
        }
        CachedBundle cb;
        cb.format = format->text();
        const json::Value* complete = fpv->find("complete");
        int64_t newest_signed = 0;
        const json::Value* nv = fpv->find("newest_ns");
        if (!get_u64(fpv->find("files"), cb.fingerprint.files) || !get_u64(fpv->find("bytes"), cb.fingerprint.bytes) ||
            !nv || !nv->is_number() || !nv->to_int64(newest_signed) || !complete || !complete->is_bool()) {
            why = "the plug-in catalog cache's fingerprint for " + bpath + " is malformed";
            out.bundles.clear();
            return CacheLoad::Discarded;
        }
        cb.fingerprint.newest_ns = newest_signed;
        cb.fingerprint.complete = complete->as_bool();
        if (const json::Value* f = v.find("failed"); f && f->is_string()) cb.failed = f->text();
        for (const json::Value& rv : plugins->items()) {
            std::string line, e;
            plugin::probe::Record r;
            // One record of another version is a miss for that plugin
            // alone, never a guess (amendment item 2).
            if (!json::write(rv, line, e) || !plugin::probe::decode(line, r, e)) continue;
            if (r.host != host) continue;
            cb.plugins.push_back(std::move(r));
        }
        out.bundles.emplace(bpath, std::move(cb));
    }
    return CacheLoad::Loaded;
}

bool save_plugin_cache(const std::string& path, const PluginCatalogCache& cache, std::string& why) {
    const std::string text = encode_plugin_cache(cache);
    if (text.size() > kMaxPluginCacheBytes) {
        why = "the plug-in catalog is " + std::to_string(text.size()) + " bytes, over the cache's " +
              std::to_string(kMaxPluginCacheBytes >> 20) + " MiB cap; it is kept in memory only";
        return false;
    }
    const fs::path target(path);
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string tmp = (dir / (target.filename().string() + ".tmp-" +
                                    std::to_string(platform::current_process_id()) + "-" +
                                    std::to_string(std::random_device{}())))
                                .string();
    std::FILE* f = platform::create_exclusive(tmp);
    if (!f) {
        why = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool synced = written && platform::sync_data(f);
    const bool closed = std::fclose(f) == 0;
    if (!written || !synced || !closed) {
        why = "cannot write " + tmp + ": " + std::strerror(errno);
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        why = "cannot replace " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    if (!platform::sync_directory(dir.string())) {
        why = "cannot sync " + dir.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace sonora
