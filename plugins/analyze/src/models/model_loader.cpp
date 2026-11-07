// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_loader.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "sonora/sha256.h"

namespace sonora::analyze {

bool check_model_bounds(const Manifest& manifest, std::string& error) {
    std::int64_t total = 0;
    for (const ManifestEntry& e : manifest.models) {
        if (e.size_bytes > kMaxModelBytes) {
            error = e.name + ": size " + std::to_string(e.size_bytes) + " exceeds the " +
                    std::to_string(kMaxModelBytes) + " byte limit per model";
            return false;
        }
        total += e.size_bytes;
        if (total > kMaxTotalModelBytes) {
            error = "manifest: models total more than " + std::to_string(kMaxTotalModelBytes) + " bytes";
            return false;
        }
    }
    return true;
}

bool FsFileDigest::digest(const std::string& path, std::int64_t& size_bytes, std::string& sha256_hex,
                          std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open";
        return false;
    }
    Sha256 h;
    std::int64_t total = 0;
    char buf[64 * 1024];
    bool ok = true;
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        total += static_cast<std::int64_t>(n);
        // An oversize file is refused without being hashed to the end.
        if (total > max_bytes_) {
            error = "file exceeds the " + std::to_string(max_bytes_) + " byte limit";
            ok = false;
            break;
        }
        h.update(buf, n);
    }
    if (ok && std::ferror(f)) {
        error = "read error";
        ok = false;
    }
    std::fclose(f);
    if (!ok) return false;
    size_bytes = total;
    sha256_hex = h.hex();
    return true;
}

bool load_manifest_file(const std::string& path, Manifest& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "manifest: cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    Manifest m;
    if (!parse_manifest(ss.str(), m, error) || !check_model_bounds(m, error)) return false;
    out = std::move(m);
    return true;
}

std::unique_ptr<ModelRunner> load_model(const Manifest& manifest, std::string_view name,
                                        const std::string& models_dir, const ModelPolicy& policy,
                                        FileDigest& digest, RunnerFactory& factory, std::string& error) {
    const ManifestEntry* e = find_model(manifest, name);
    if (!e) {
        error = std::string(name) + ": not in the manifest";
        return nullptr;
    }
    if (!check_model_bounds(manifest, error) || !verify_file(*e, models_dir, policy, digest, error)) return nullptr;
    const std::string path = models_dir.empty() ? e->file : models_dir + "/" + e->file;
    return factory.open(*e, path, error);
}

}  // namespace sonora::analyze
