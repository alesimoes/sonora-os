// SPDX-License-Identifier: GPL-3.0-or-later
#include "keymap_profiles.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "key_router.h"
#include "sonora/keymap.h"

namespace fs = std::filesystem;

namespace sonora::ui {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

bool is_dir(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

ProfileEntry entry_for(const std::string& file) {
    ProfileEntry e;
    e.file = file;
    KeyMap map;
    std::string err;
    if (KeyMap::load_file(file, map, err)) {
        e.name = map.name();
    } else {
        e.name = fs::path(file).filename().string();
        e.error = err;
    }
    return e;
}

}  // namespace

std::string profile_path(const std::string& file) {
    if (file.empty()) return std::string();
    std::error_code ec;
    fs::path p = fs::absolute(file, ec);
    if (ec) p = fs::path(file);
    return p.lexically_normal().string();
}

std::vector<ProfileEntry> list_profiles(const std::string& dir, const std::string& extra) {
    std::vector<ProfileEntry> files;
    std::error_code ec;
    if (is_dir(dir)) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".json") continue;
            std::error_code fe;
            if (!it->is_regular_file(fe)) continue;
            files.push_back(entry_for(profile_path(it->path().string())));
        }
    }
    if (!extra.empty()) {
        const std::string abs = profile_path(extra);
        const bool listed = std::any_of(files.begin(), files.end(),
                                        [&](const ProfileEntry& e) {
                                            std::error_code se;
                                            return fs::equivalent(e.file, abs, se);
                                        });
        if (!listed) files.push_back(entry_for(abs));
    }
    std::sort(files.begin(), files.end(), [](const ProfileEntry& a, const ProfileEntry& b) {
        return a.name != b.name ? a.name < b.name : a.file < b.file;
    });

    const std::string builtin = KeyMap::built_in_default().name();
    const bool shadowed = std::any_of(files.begin(), files.end(), [&](const ProfileEntry& e) {
        return e.error.empty() && e.name == builtin;
    });
    std::vector<ProfileEntry> out;
    if (!shadowed) out.push_back(ProfileEntry{std::string(), builtin, std::string()});
    out.insert(out.end(), files.begin(), files.end());
    return out;
}

std::string keymap_dir(const std::string& given, const std::string& keymap_path,
                       const std::string& exe_dir) {
    if (!given.empty()) return is_dir(given) ? given : std::string();
    const std::string from_env = env("SONORA_KEYMAP_DIR");
    if (is_dir(from_env)) return from_env;
    if (!keymap_path.empty()) {
        const std::string parent = fs::path(keymap_path).parent_path().string();
        if (is_dir(parent.empty() ? "." : parent)) return parent.empty() ? "." : parent;
    }
    if (!exe_dir.empty()) {
        const std::string installed =
            (fs::path(exe_dir) / ".." / "share" / "sonora" / "keymaps").lexically_normal().string();
        if (is_dir(installed)) return installed;
    }
    const std::string system = "/usr/share/sonora/keymaps";
    return is_dir(system) ? system : std::string();
}

std::string remembered_profile_path(const std::string& config_home) {
    std::string base = config_home;
    if (base.empty()) {
        base = env("XDG_CONFIG_HOME");
        if (!fs::path(base).is_absolute()) base.clear();
    }
    if (base.empty()) {
        const std::string home = env("HOME");
        if (home.empty()) return std::string();
        base = (fs::path(home) / ".config").string();
    }
    return (fs::path(base) / "sonora" / "keymap").string();
}

std::string remembered_profile(const std::string& path, const std::string& dir) {
    if (path.empty()) return std::string();
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line)) return std::string();
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) return std::string();
    const fs::path p(line);
    if (p.is_absolute() || dir.empty()) return line;
    return (fs::path(dir) / p).string();
}

bool remember_profile(const std::string& path, const std::string& dir, const std::string& file,
                      std::string& why) {
    if (path.empty()) {
        why = "no configuration folder is known (neither XDG_CONFIG_HOME nor HOME is set)";
        return false;
    }
    std::string text = file;
    std::error_code ec;
    if (!file.empty() && !dir.empty()) {
        const fs::path f = fs::absolute(file, ec);
        if (!ec && fs::equivalent(f.parent_path(), fs::path(dir), ec) && !ec) {
            text = f.filename().string();
        }
    }
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        why = "cannot create " + fs::path(path).parent_path().string() + ": " + ec.message();
        return false;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << text << "\n";
        out.flush();
        if (!out) {
            why = "cannot write " + tmp;
            std::remove(tmp.c_str());
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        why = "cannot replace " + path + ": " + ec.message();
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

StartupProfile startup_profile(const std::string& keymap_arg, const std::string& remembered_at,
                               const std::string& dir) {
    StartupProfile s;
    std::string file = keymap_arg;
    if (file.empty()) file = remembered_profile(remembered_at, dir);
    s.file = profile_path(file);
    s.map = keymap_or_built_in(s.file, s.refusal);
    if (!s.refusal.empty()) s.file.clear();
    return s;
}

}  // namespace sonora::ui
