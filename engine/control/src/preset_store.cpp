// SPDX-License-Identifier: GPL-3.0-or-later
// The user preset store (preset_store.h). Control thread only: files,
// fsync, allocation.
#include "sonora/preset_store.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

#include "sonora/json/json.h"
#include "sonora/platform/file.h"
#include "sonora/platform/process.h"
#include "sonora/plugin/host_control.h"
#include "sonora/sha256.h"

namespace sonora {

namespace fs = std::filesystem;
namespace pc = plugin::control;

namespace {

constexpr const char* kSidecarSuffix = ".preset.json";
constexpr const char* kTempMark = ".tmp-";
constexpr const char* kStateDir = "Plugin State";
constexpr const char* kFilesDir = "Plugin Files";
constexpr const char* kPluginFile = "plugin.json";
// A plugin id is a URI or a VST3 class id; this only bounds a hostile one.
constexpr std::size_t kMaxPluginTextBytes = 4096;
constexpr std::size_t kMaxBasenameBytes = 255;

bool is_lower_hex(std::string_view s, std::size_t n) {
    if (s.size() != n) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

bool has_control(std::string_view s) {
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

// What a menu can show and the sidecar can hold.
bool valid_name(const std::string& name, std::string& why) {
    if (name.size() > kMaxPresetNameBytes) {
        why = "a preset name is at most " + std::to_string(kMaxPresetNameBytes) + " bytes";
        return false;
    }
    if (!json::valid_utf8(name) || has_control(name)) {
        why = "a preset name must be text on one line";
        return false;
    }
    if (name.find_first_not_of(" \t") == std::string::npos) {
        why = "a preset needs a name";
        return false;
    }
    return true;
}

bool valid_text(const std::string& s) { return s.size() <= kMaxPluginTextBytes && json::valid_utf8(s); }

// One path component: nothing that climbs out of its folder or names a
// folder of its own, and nothing cleanup could take for a temporary (#275
// review B1).
bool valid_basename(std::string_view b) {
    if (b.empty() || b.size() > kMaxBasenameBytes || b == "." || b == "..") return false;
    if (b.find('/') != std::string_view::npos || b.find('\\') != std::string_view::npos) return false;
    if (b.find(kTempMark) != std::string_view::npos) return false;
    return json::valid_utf8(b) && !has_control(b);
}

bool known_format(std::string_view f, pc::Format& out) {
    if (f == "lv2") out = pc::Format::Lv2;
    else if (f == "vst3") out = pc::Format::Vst3;
    else return false;
    return true;
}

// "Plugin Files/<sha256>/<basename>" whose hash is `sha256`.
bool valid_file_path(std::string_view path, std::string_view sha256) {
    const std::string head = std::string(kFilesDir) + "/";
    if (path.substr(0, head.size()) != head) return false;
    path.remove_prefix(head.size());
    if (path.size() < 66 || path.substr(0, 64) != sha256 || path[64] != '/') return false;
    return valid_basename(path.substr(65));
}

std::string temp_name(const std::string& target) {
    return target + kTempMark + std::to_string(platform::current_process_id()) + "-" +
           std::to_string(std::random_device{}());
}

enum class Read { Ok, Missing, TooBig, Error };

// A file that is not regular (a folder, a device, a FIFO that would block)
// is an error, and its size is checked before a byte is read.
Read read_capped(const fs::path& path, uint64_t cap, std::string& out) {
    out.clear();
    std::error_code ec;
    const fs::file_status st = fs::status(path, ec);
    if (ec || !fs::exists(st)) return ec && ec != std::errc::no_such_file_or_directory ? Read::Error : Read::Missing;
    if (!fs::is_regular_file(st)) return Read::Error;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) return Read::Error;
    if (size > cap) return Read::TooBig;
    std::ifstream f(path, std::ios::binary);
    if (!f) return Read::Error;
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !f.read(&out[0], static_cast<std::streamsize>(size))) return Read::Error;
    // A file that grew since its size was read is read no further: what was
    // read is checked against its hash anyway.
    return Read::Ok;
}

// tmp, sync, rename, sync the folder: a power cut leaves the old file or the
// new one, never part of either.
bool write_durably(const fs::path& target, const std::string& bytes, std::string& why) {
    const fs::path dir = target.parent_path();
    const std::string tmp = temp_name(target.string());
    std::FILE* f = platform::create_exclusive(tmp);
    if (!f) {
        why = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool synced = written && platform::sync_data(f);
    const int err = errno;
    const bool closed = std::fclose(f) == 0;
    std::error_code ec;
    if (!written || !synced || !closed) {
        why = "cannot write " + tmp + ": " + std::strerror(written && synced ? errno : err);
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        why = "cannot replace " + target.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    if (!platform::sync_directory(dir.string())) {
        why = "cannot sync " + dir.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

// Content-addressed: a file already holding these bytes is left as it is,
// so two presets of one state share it and nothing is rewritten.
bool put_content(const fs::path& target, const std::string& bytes, std::string& why) {
    std::string have;
    if (read_capped(target, bytes.size(), have) == Read::Ok && have == bytes) return true;
    return write_durably(target, bytes, why);
}

bool make_dir(const fs::path& dir, std::string& why) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        why = "cannot make the folder " + dir.string() + (ec ? ": " + ec.message() : "");
        return false;
    }
    return true;
}

const json::Value* field(const json::Value& o, std::string_view key, json::Value::Type type) {
    const json::Value* v = o.find(key);
    return v && v->type() == type ? v : nullptr;
}

bool get_text(const json::Value& o, std::string_view key, std::string& out) {
    const json::Value* v = field(o, key, json::Value::Type::String);
    if (!v) return false;
    out = v->text();
    return true;
}

bool get_size(const json::Value& o, std::string_view key, uint64_t cap, uint64_t& out) {
    const json::Value* v = field(o, key, json::Value::Type::Number);
    int64_t n = 0;
    if (!v || !v->to_int64(n) || n < 0 || uint64_t(n) > cap) return false;
    out = uint64_t(n);
    return true;
}

std::string random_preset_id() {
    std::random_device rd;
    char hex[33];
    for (int i = 0; i < 4; ++i) std::snprintf(hex + 8 * i, 9, "%08x", static_cast<unsigned>(rd()));
    return "u-" + std::string(hex, 32);
}

std::string encode_plugin_file(const PresetPlugin& p) {
    json::Value o = json::Value::object();
    o.set("format", json::Value::string(p.format));
    o.set("id", json::Value::string(p.id));
    o.set("name", json::Value::string(p.name));
    o.set("vendor", json::Value::string(p.vendor));
    std::string out, e;
    json::write(o, out, e);
    return out;
}

// plugin.json, when it reads as this folder's plugin.
bool read_plugin_file(const fs::path& dir, const std::string& key, PresetPlugin& out) {
    std::string text;
    if (read_capped(dir / kPluginFile, kMaxPresetPluginFileBytes, text) != Read::Ok) return false;
    json::Value doc;
    json::Error je;
    json::Limits limits;
    limits.max_input_bytes = kMaxPresetPluginFileBytes;
    limits.max_depth = 4;
    if (!json::parse(text, doc, je, limits) || !doc.is_object()) return false;
    PresetPlugin p;
    pc::Format f;
    if (!get_text(doc, "format", p.format) || !known_format(p.format, f) || !get_text(doc, "id", p.id) ||
        p.id.empty() || preset_plugin_key(p.format, p.id) != key)
        return false;
    get_text(doc, "name", p.name);
    get_text(doc, "vendor", p.vendor);
    out = std::move(p);
    return true;
}

struct FolderEntry {
    std::string id;
    fs::path path;
};

// The sidecars of one plugin folder, temporaries left out. `cut` when there
// are more than the cap; `all_named` false when a file ends in the suffix
// without a user id before it.
std::vector<FolderEntry> sidecar_entries(const fs::path& dir, bool& cut, bool& all_named) {
    std::vector<FolderEntry> out;
    cut = false;
    all_named = true;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!ends_with(name, kSidecarSuffix) || name.find(kTempMark) != std::string::npos) continue;
        const std::string id = name.substr(0, name.size() - std::strlen(kSidecarSuffix));
        if (!is_user_preset_id(id)) {
            all_named = false;
            continue;
        }
        if (out.size() == kMaxPresetsPerPlugin) {
            cut = true;
            break;
        }
        out.push_back({id, it->path()});
    }
    if (ec) cut = true;
    return out;
}

// Reads one sidecar and checks it belongs where it was found.
PresetResult read_sidecar(const fs::path& path, const std::string& key, const std::string& id,
                          PresetSidecar& out, std::string& why) {
    std::string text;
    switch (read_capped(path, kMaxPresetSidecarBytes, text)) {
    case Read::Ok: break;
    case Read::Missing: why = "no such preset"; return PresetResult::NotFound;
    case Read::TooBig:
        why = "preset damaged: its sidecar is over the " + std::to_string(kMaxPresetSidecarBytes >> 10) +
              " KiB cap";
        return PresetResult::Damaged;
    case Read::Error: why = "preset damaged: its sidecar cannot be read"; return PresetResult::Damaged;
    }
    std::string e;
    switch (decode_preset_sidecar(text, out, e)) {
    case SidecarRead::Ok: break;
    case SidecarRead::Newer: why = e; return PresetResult::Newer;
    case SidecarRead::Malformed: why = "preset damaged: " + e; return PresetResult::Damaged;
    }
    if (out.id != id) {
        why = "preset damaged: its sidecar names another preset";
        return PresetResult::Damaged;
    }
    if (preset_plugin_key(out.plugin.format, out.plugin.id) != key) {
        why = "preset damaged: its sidecar is in another plugin's folder";
        return PresetResult::Damaged;
    }
    return PresetResult::Ok;
}

// Cleanup deletes only inside folders the store made and only files it
// wrote: a symlink, to a folder or to a file, is never followed and never
// removed (#275 review B2).
bool real_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec));
}

bool real_dir(const fs::directory_entry& e) {
    std::error_code ec;
    return fs::is_directory(e.symlink_status(ec));
}

bool real_file(const fs::directory_entry& e) {
    std::error_code ec;
    return fs::is_regular_file(e.symlink_status(ec));
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// The exact name temp_name() gives a target, "<target>.tmp-<pid>-<n>",
// never a name that merely holds the mark.
bool temp_target(std::string_view name, std::string& target) {
    const std::size_t at = name.rfind(kTempMark);
    if (at == std::string_view::npos || at == 0) return false;
    const std::string_view tail = name.substr(at + std::strlen(kTempMark));
    const std::size_t dash = tail.find('-');
    if (dash == std::string_view::npos || !all_digits(tail.substr(0, dash)) || !all_digits(tail.substr(dash + 1)))
        return false;
    target = std::string(name.substr(0, at));
    return true;
}

bool is_state_name(std::string_view name) {
    return ends_with(name, ".state") && is_lower_hex(name.substr(0, name.size() - 6), 64);
}

bool is_sidecar_name(std::string_view name) {
    return ends_with(name, kSidecarSuffix) &&
           is_user_preset_id(name.substr(0, name.size() - std::strlen(kSidecarSuffix)));
}

// Removes the temporaries a save left directly in `dir` (no recursion) for
// targets that belong there.
template <class Ours>
void remove_temporaries(const fs::path& dir, const Ours& ours, PresetCleanup& report) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        std::string target;
        std::error_code e2;
        if (real_file(*it) && temp_target(it->path().filename().string(), target) && ours(target) &&
            fs::remove(it->path(), e2))
            ++report.temporaries_removed;
    }
}

}  // namespace

std::string user_preset_dir() {
    const std::string data = platform::user_data_dir();
    return data.empty() ? std::string() : (fs::path(data) / "presets").string();
}

std::string preset_plugin_key(std::string_view format, std::string_view id) {
    std::string text(format);
    text += ':';
    text += id;
    return sha256_hex(text).substr(0, 32);
}

bool is_user_preset_id(std::string_view id) {
    return id.size() == 34 && id.substr(0, 2) == "u-" && is_lower_hex(id.substr(2), 32);
}

bool encode_preset_sidecar(const PresetSidecar& s, std::string& out, std::string& error) {
    json::Value o = json::Value::object();
    o.set("sonora_preset", json::Value::integer(kPresetFormatVersion));
    o.set("id", json::Value::string(s.id));
    o.set("name", json::Value::string(s.name));
    json::Value p = json::Value::object();
    p.set("format", json::Value::string(s.plugin.format));
    p.set("id", json::Value::string(s.plugin.id));
    p.set("name", json::Value::string(s.plugin.name));
    p.set("version", json::Value::string(s.plugin.version));
    o.set("plugin", std::move(p));
    json::Value params = json::Value::array();
    for (const PresetParam& pp : s.params) {
        json::Value v = json::Value::object();
        v.set("key", json::Value::string(pp.key));
        v.set("value", json::Value::number_float(pp.value));
        params.push(std::move(v));
    }
    o.set("params", std::move(params));
    json::Value st = json::Value::object();
    st.set("file", json::Value::string(s.state_file));
    st.set("sha256", json::Value::string(s.state_sha256));
    st.set("bytes", json::Value::integer(static_cast<int64_t>(s.state_bytes)));
    json::Value files = json::Value::array();
    for (const PresetFileRef& f : s.files) {
        json::Value v = json::Value::object();
        v.set("path", json::Value::string(f.path));
        v.set("sha256", json::Value::string(f.sha256));
        v.set("bytes", json::Value::integer(static_cast<int64_t>(f.bytes)));
        files.push(std::move(v));
    }
    st.set("files", std::move(files));
    o.set("state", std::move(st));
    o.set("category", json::Value::string(s.category));
    out.clear();
    return json::write(o, out, error);
}

SidecarRead decode_preset_sidecar(std::string_view text, PresetSidecar& out, std::string& error) {
    out = PresetSidecar{};
    json::Value doc;
    json::Error je;
    json::Limits limits;
    limits.max_input_bytes = kMaxPresetSidecarBytes;
    limits.max_depth = 8;
    limits.max_values = 8 * kMaxPresetParams + 16 * kMaxPresetFiles + 256;
    limits.max_dom_bytes = std::size_t{16} << 20;
    if (!json::parse(text, doc, je, limits)) {
        error = "not readable JSON (" + je.message + ", line " + std::to_string(je.line) + ")";
        return SidecarRead::Malformed;
    }
    if (!doc.is_object()) {
        error = "not a preset sidecar";
        return SidecarRead::Malformed;
    }
    const json::Value* version = field(doc, "sonora_preset", json::Value::Type::Number);
    int64_t v = 0;
    if (!version || !version->to_int64(v) || v < 1) {
        error = "not a preset sidecar";
        return SidecarRead::Malformed;
    }
    if (v != kPresetFormatVersion) {
        error = "made by a newer Sonora (sonora_preset " + std::to_string(v) + ")";
        return SidecarRead::Newer;
    }
    PresetSidecar s;
    std::string why;
    if (!get_text(doc, "id", s.id) || !is_user_preset_id(s.id)) {
        error = "no user preset id";
        return SidecarRead::Malformed;
    }
    if (!get_text(doc, "name", s.name) || !valid_name(s.name, why)) {
        error = "no valid name";
        return SidecarRead::Malformed;
    }
    const json::Value* p = field(doc, "plugin", json::Value::Type::Object);
    pc::Format f;
    if (!p || !get_text(*p, "format", s.plugin.format) || !known_format(s.plugin.format, f) ||
        !get_text(*p, "id", s.plugin.id) || s.plugin.id.empty() || !valid_text(s.plugin.id) ||
        !get_text(*p, "name", s.plugin.name) || !valid_text(s.plugin.name) ||
        !get_text(*p, "version", s.plugin.version) || !valid_text(s.plugin.version)) {
        error = "no valid plugin identity";
        return SidecarRead::Malformed;
    }
    const json::Value* params = field(doc, "params", json::Value::Type::Array);
    if (!params || params->items().size() > kMaxPresetParams) {
        error = "no valid params";
        return SidecarRead::Malformed;
    }
    for (const json::Value& pv : params->items()) {
        PresetParam pp;
        const json::Value* value = pv.is_object() ? field(pv, "value", json::Value::Type::Number) : nullptr;
        if (!value || !get_text(pv, "key", pp.key) || pp.key.empty() || !valid_text(pp.key) ||
            !value->to_float(pp.value) || !std::isfinite(pp.value)) {
            error = "a malformed parameter";
            return SidecarRead::Malformed;
        }
        s.params.push_back(std::move(pp));
    }
    const json::Value* st = field(doc, "state", json::Value::Type::Object);
    if (!st || !get_text(*st, "sha256", s.state_sha256) || !is_lower_hex(s.state_sha256, 64) ||
        !get_text(*st, "file", s.state_file) || s.state_file != pc::state_file_name(s.state_sha256) ||
        !get_size(*st, "bytes", pc::kMaxStateBytes, s.state_bytes)) {
        error = "no valid state";
        return SidecarRead::Malformed;
    }
    const json::Value* files = field(*st, "files", json::Value::Type::Array);
    if (!files || files->items().size() > kMaxPresetFiles) {
        error = "no valid state files";
        return SidecarRead::Malformed;
    }
    for (const json::Value& fv : files->items()) {
        PresetFileRef r;
        if (!fv.is_object() || !get_text(fv, "sha256", r.sha256) || !is_lower_hex(r.sha256, 64) ||
            !get_text(fv, "path", r.path) || !valid_file_path(r.path, r.sha256) ||
            !get_size(fv, "bytes", kMaxPresetFileBytes, r.bytes)) {
            error = "a malformed state file";
            return SidecarRead::Malformed;
        }
        s.files.push_back(std::move(r));
    }
    if (const json::Value* c = doc.find("category")) {
        if (!c->is_string() || !valid_text(c->text())) {
            error = "a malformed category";
            return SidecarRead::Malformed;
        }
        s.category = c->text();
    }
    out = std::move(s);
    return SidecarRead::Ok;
}

PresetStore::PresetStore(std::string root) : root_(std::move(root)) {}

bool PresetStore::open(std::string& error) {
    if (root_.empty()) {
        error = "no folder for this user's data, so no user presets";
        return false;
    }
    if (!make_dir(root_, error)) return false;
    if (!cleaned_) {
        cleaned_ = true;
        cleanup();
    }
    return true;
}

ListedPlugin PresetStore::list_folder(const std::string& key) const {
    ListedPlugin out;
    out.key = key;
    if (root_.empty()) return out;
    const fs::path dir = fs::path(root_) / key;
    bool all_named = true;
    const std::vector<FolderEntry> entries = sidecar_entries(dir, out.cut, all_named);
    const bool have_plugin = read_plugin_file(dir, key, out.plugin);
    for (const FolderEntry& e : entries) {
        ListedPreset lp;
        lp.id = e.id;
        const PresetResult res = read_sidecar(e.path, key, e.id, lp.sidecar, lp.why);
        if (res == PresetResult::Ok) {
            // A blob found damaged at load this run; its sidecar still
            // gives the name the menu shows beside "damaged".
            if (damaged_.count(key + "/" + e.id)) {
                lp.state = PresetState::Damaged;
                lp.why = "preset damaged: its state does not match its sidecar";
            }
            if (!have_plugin && out.plugin.id.empty()) out.plugin = lp.sidecar.plugin;
        } else {
            lp.state = res == PresetResult::Newer ? PresetState::Newer : PresetState::Damaged;
            lp.sidecar = PresetSidecar{};
        }
        out.presets.push_back(std::move(lp));
    }
    // Named presets by name, the unreadable after them by id.
    std::sort(out.presets.begin(), out.presets.end(), [](const ListedPreset& a, const ListedPreset& b) {
        const bool ra = !a.sidecar.name.empty();
        const bool rb = !b.sidecar.name.empty();
        if (ra != rb) return ra;
        if (ra && a.sidecar.name != b.sidecar.name) return a.sidecar.name < b.sidecar.name;
        return a.id < b.id;
    });
    return out;
}

ListedPlugin PresetStore::list(std::string_view format, std::string_view plugin_id) const {
    ListedPlugin out = list_folder(preset_plugin_key(format, plugin_id));
    if (out.plugin.id.empty()) {
        out.plugin.format = std::string(format);
        out.plugin.id = std::string(plugin_id);
    }
    return out;
}

std::vector<ListedPlugin> PresetStore::list_all(
    const std::function<bool(const std::string& format, const std::string& id)>& installed) const {
    std::vector<ListedPlugin> out;
    if (root_.empty()) return out;
    std::error_code ec;
    fs::directory_iterator it(root_, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string key = it->path().filename().string();
        if (!is_lower_hex(key, 32) || !real_dir(*it)) continue;
        ListedPlugin l = list_folder(key);
        if (l.presets.empty()) continue;
        l.plugin_missing = l.plugin.id.empty() || !installed || !installed(l.plugin.format, l.plugin.id);
        out.push_back(std::move(l));
    }
    std::sort(out.begin(), out.end(), [](const ListedPlugin& a, const ListedPlugin& b) {
        return a.plugin.name != b.plugin.name ? a.plugin.name < b.plugin.name : a.key < b.key;
    });
    return out;
}

bool PresetStore::no_root(std::string& why) const {
    // An empty root would resolve every path against the working directory.
    if (!root_.empty()) return false;
    why = "no folder for this user's data, so no user presets";
    return true;
}

PresetResult PresetStore::save(const PresetSave& r, std::string& id_out, std::string& why) {
    if (no_root(why)) return PresetResult::IoError;
    pc::Format format;
    if (!known_format(r.plugin.format, format)) {
        why = "presets are kept for LV2 and VST3 plugins only";
        return PresetResult::Invalid;
    }
    if (r.plugin.id.empty() || !valid_text(r.plugin.id) || !valid_text(r.plugin.name) ||
        !valid_text(r.plugin.vendor) || !valid_text(r.plugin.version) || !valid_text(r.category)) {
        why = "the plugin's identity is not text a preset can hold";
        return PresetResult::Invalid;
    }
    if (!valid_name(r.name, why)) return PresetResult::Invalid;
    if (r.params.size() > kMaxPresetParams) {
        why = "a preset holds at most " + std::to_string(kMaxPresetParams) + " parameters";
        return PresetResult::Invalid;
    }
    for (const PresetParam& p : r.params) {
        if (p.key.empty() || !valid_text(p.key) || !std::isfinite(p.value)) {
            why = "a parameter without a key or a finite value";
            return PresetResult::Invalid;
        }
    }
    if (r.files.size() > kMaxPresetFiles) {
        why = "a preset holds at most " + std::to_string(kMaxPresetFiles) + " files";
        return PresetResult::Invalid;
    }
    for (const PresetFileSource& f : r.files) {
        if (!valid_basename(f.basename)) {
            why = "a state file's name is not one plain file name";
            return PresetResult::Invalid;
        }
    }
    if (r.blob.size() > pc::kMaxStateBytes) {
        why = "the plugin's state is over the " + std::to_string(pc::kMaxStateBytes >> 20) + " MiB cap";
        return PresetResult::Invalid;
    }
    pc::StateBlob blob;
    std::string e;
    if (!pc::decode_state_blob(r.blob, blob, e)) {
        why = "the plugin's state is not a state blob: " + e;
        return PresetResult::Invalid;
    }
    if (blob.format != format || blob.id != r.plugin.id) {
        why = "the state is another plugin's";
        return PresetResult::Invalid;
    }

    const std::string key = preset_plugin_key(r.plugin.format, r.plugin.id);
    const fs::path dir = fs::path(root_) / key;
    if (!r.overwrite_id.empty()) {
        if (!is_user_preset_id(r.overwrite_id)) {
            why = "no such preset";
            return PresetResult::NotFound;
        }
        PresetSidecar old;
        const PresetResult res = read_sidecar(dir / (r.overwrite_id + kSidecarSuffix), key, r.overwrite_id, old, why);
        // A damaged preset may be overwritten: that is how it is repaired.
        if (res == PresetResult::NotFound || res == PresetResult::Newer) return res;
    }
    const ListedPlugin listed = list_folder(key);
    for (const ListedPreset& p : listed.presets) {
        // An unreadable sidecar's name is unknown (its sidecar is cleared),
        // so it blocks nothing; one damaged only in its blob still does.
        if (p.id != r.overwrite_id && p.sidecar.name == r.name) {
            why = "a preset of this plugin is already called \"" + r.name + "\"";
            return PresetResult::NameTaken;
        }
    }

    // Everything the sidecar names is durable before the sidecar exists.
    if (!make_dir(dir / kStateDir, why)) return PresetResult::IoError;
    // Never written through a link: the store's folders are its own.
    const auto linked = [&](const fs::path& p) {
        why = p.string() + " is not a folder of the preset store (a link?); nothing was written";
        return PresetResult::IoError;
    };
    if (!real_dir(dir)) return linked(dir);
    if (!real_dir(dir / kStateDir)) return linked(dir / kStateDir);
    PresetSidecar s;
    for (const PresetFileSource& f : r.files) {
        std::string bytes;
        switch (read_capped(f.source, kMaxPresetFileBytes, bytes)) {
        case Read::Ok: break;
        case Read::TooBig:
            why = "the state's file " + f.source + " is over the " + std::to_string(kMaxPresetFileBytes >> 20) +
                  " MiB cap of a preset file";
            return PresetResult::Invalid;
        default: why = "cannot read the state's file " + f.source; return PresetResult::IoError;
        }
        PresetFileRef ref;
        ref.sha256 = sha256_hex(bytes);
        ref.path = std::string(kFilesDir) + "/" + ref.sha256 + "/" + f.basename;
        ref.bytes = bytes.size();
        const fs::path target = dir / kFilesDir / ref.sha256 / f.basename;
        if (!make_dir(target.parent_path(), why)) return PresetResult::IoError;
        if (!real_dir(dir / kFilesDir)) return linked(dir / kFilesDir);
        if (!real_dir(target.parent_path())) return linked(target.parent_path());
        if (!put_content(target, bytes, why)) return PresetResult::IoError;
        s.files.push_back(std::move(ref));
    }
    s.state_sha256 = sha256_hex(r.blob);
    s.state_file = pc::state_file_name(s.state_sha256);
    s.state_bytes = r.blob.size();
    if (!put_content(dir / s.state_file, r.blob, why)) return PresetResult::IoError;
    // The name last seen, for listing the plugin once it is gone (§4).
    PresetPlugin seen{r.plugin.format, r.plugin.id, r.plugin.name, r.plugin.vendor, {}};
    if (!put_content(dir / kPluginFile, encode_plugin_file(seen), why)) return PresetResult::IoError;

    s.id = r.overwrite_id;
    if (s.id.empty()) {
        std::error_code ec;
        do s.id = random_preset_id();
        while (fs::exists(fs::symlink_status(dir / (s.id + kSidecarSuffix), ec)));
    }
    s.name = r.name;
    s.plugin = r.plugin;
    s.plugin.vendor.clear();
    s.params = r.params;
    s.category = r.category;
    std::string text;
    if (!encode_preset_sidecar(s, text, why)) return PresetResult::Invalid;
    if (!write_durably(dir / (s.id + kSidecarSuffix), text, why)) return PresetResult::IoError;
    damaged_.erase(key + "/" + s.id);
    id_out = s.id;
    return PresetResult::Ok;
}

PresetResult PresetStore::rename(std::string_view format, std::string_view plugin_id, std::string_view preset_id,
                                 const std::string& new_name, std::string& why) {
    if (no_root(why)) return PresetResult::IoError;
    if (!valid_name(new_name, why)) return PresetResult::Invalid;
    if (!is_user_preset_id(preset_id)) {
        why = "no such preset";
        return PresetResult::NotFound;
    }
    const std::string key = preset_plugin_key(format, plugin_id);
    const fs::path dir = fs::path(root_) / key;
    const std::string id(preset_id);
    PresetSidecar s;
    const PresetResult res = read_sidecar(dir / (id + kSidecarSuffix), key, id, s, why);
    if (res != PresetResult::Ok) return res;
    if (s.name == new_name) return PresetResult::Ok;
    for (const ListedPreset& p : list_folder(key).presets) {
        if (p.id != id && p.sidecar.name == new_name) {
            why = "a preset of this plugin is already called \"" + new_name + "\"";
            return PresetResult::NameTaken;
        }
    }
    s.name = new_name;
    std::string text;
    if (!encode_preset_sidecar(s, text, why)) return PresetResult::Invalid;
    return write_durably(dir / (id + kSidecarSuffix), text, why) ? PresetResult::Ok : PresetResult::IoError;
}

PresetResult PresetStore::remove(std::string_view format, std::string_view plugin_id, std::string_view preset_id,
                                 std::string& why) {
    if (no_root(why)) return PresetResult::IoError;
    if (!is_user_preset_id(preset_id)) {
        why = "no such preset";
        return PresetResult::NotFound;
    }
    const std::string key = preset_plugin_key(format, plugin_id);
    const fs::path dir = fs::path(root_) / key;
    const fs::path path = dir / (std::string(preset_id) + kSidecarSuffix);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(path, ec))) {
        why = "no such preset";
        return PresetResult::NotFound;
    }
    if (!fs::remove(path, ec) || ec) {
        why = "cannot delete " + path.string() + (ec ? ": " + ec.message() : "");
        return PresetResult::IoError;
    }
    damaged_.erase(key + "/" + std::string(preset_id));
    if (!platform::sync_directory(dir.string())) {
        why = "cannot sync " + dir.string() + ": " + std::strerror(errno);
        return PresetResult::IoError;
    }
    return PresetResult::Ok;
}

PresetResult PresetStore::load(std::string_view format, std::string_view plugin_id, std::string_view preset_id,
                               LoadedPreset& out, std::string& why) {
    out = LoadedPreset{};
    if (no_root(why)) return PresetResult::IoError;
    if (!is_user_preset_id(preset_id)) {
        why = "no such preset";
        return PresetResult::NotFound;
    }
    const std::string key = preset_plugin_key(format, plugin_id);
    const fs::path dir = fs::path(root_) / key;
    const std::string id(preset_id);
    PresetSidecar s;
    const PresetResult res = read_sidecar(dir / (id + kSidecarSuffix), key, id, s, why);
    if (res != PresetResult::Ok) return res;
    const auto damaged = [&](std::string what) {
        damaged_.insert(key + "/" + id);
        why = "preset damaged: " + std::move(what);
        return PresetResult::Damaged;
    };
    std::string blob;
    if (read_capped(dir / s.state_file, s.state_bytes, blob) != Read::Ok || blob.size() != s.state_bytes ||
        !pc::state_matches(blob, s.state_sha256))
        return damaged("its state is missing or does not match its hash");
    pc::StateBlob decoded;
    pc::Format f;
    std::string e;
    if (!pc::decode_state_blob(blob, decoded, e) || !known_format(s.plugin.format, f) || decoded.format != f ||
        decoded.id != s.plugin.id)
        return damaged("its state is not this plugin's");
    std::vector<std::string> paths;
    for (const PresetFileRef& r : s.files) {
        std::string bytes;
        if (read_capped(dir / r.path, r.bytes, bytes) != Read::Ok || bytes.size() != r.bytes ||
            sha256_hex(bytes) != r.sha256)
            return damaged("its file " + r.path + " is missing or does not match its hash");
        paths.push_back((dir / r.path).string());
    }
    damaged_.erase(key + "/" + id);
    out.sidecar = std::move(s);
    out.blob = std::move(blob);
    out.file_paths = std::move(paths);
    return PresetResult::Ok;
}

PresetCleanup PresetStore::cleanup() {
    PresetCleanup report;
    if (root_.empty()) return report;
    std::error_code ec;
    fs::directory_iterator it(root_, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string key = it->path().filename().string();
        std::error_code e2;
        if (!is_lower_hex(key, 32) || !real_dir(*it)) continue;
        const fs::path dir = it->path();
        const bool state_dir = real_dir(dir / kStateDir);
        const bool files_dir = real_dir(dir / kFilesDir);
        // A temporary is a save that never reached its rename: nothing names it.
        remove_temporaries(
            dir, [](const std::string& t) { return t == kPluginFile || is_sidecar_name(t); }, report);
        if (state_dir) remove_temporaries(dir / kStateDir, [](const std::string& t) { return is_state_name(t); }, report);
        if (files_dir) {
            fs::directory_iterator fd(dir / kFilesDir, e2);
            for (const fs::directory_iterator end2; !e2 && fd != end2; fd.increment(e2)) {
                if (is_lower_hex(fd->path().filename().string(), 64) && real_dir(*fd))
                    remove_temporaries(fd->path(), [](const std::string& t) { return valid_basename(t); }, report);
            }
        }
        bool cut = false, all_named = true;
        std::set<std::string> named;
        bool readable = true;
        for (const FolderEntry& entry : sidecar_entries(dir, cut, all_named)) {
            PresetSidecar s;
            std::string why;
            if (read_sidecar(entry.path, key, entry.id, s, why) != PresetResult::Ok) {
                readable = false;
                break;
            }
            named.insert(s.state_file);
            for (const PresetFileRef& f : s.files) named.insert(f.path);
        }
        if (!readable || cut || !all_named) {
            ++report.folders_kept;
            continue;
        }
        // Only names this store writes are removed; anything else is left.
        if (state_dir) {
            fs::directory_iterator st(dir / kStateDir, e2);
            for (const fs::directory_iterator end2; !e2 && st != end2; st.increment(e2)) {
                const std::string name = st->path().filename().string();
                std::error_code e3;
                if (!is_state_name(name) || !real_file(*st)) continue;
                if (!named.count(std::string(kStateDir) + "/" + name) && fs::remove(st->path(), e3))
                    ++report.blobs_removed;
            }
            platform::sync_directory((dir / kStateDir).string());
        }
        if (!files_dir) continue;
        fs::directory_iterator fd(dir / kFilesDir, e2);
        for (const fs::directory_iterator end2; !e2 && fd != end2; fd.increment(e2)) {
            const std::string sha = fd->path().filename().string();
            std::error_code e3;
            if (!is_lower_hex(sha, 64) || !real_dir(*fd)) continue;
            fs::directory_iterator ff(fd->path(), e3);
            for (const fs::directory_iterator end3; !e3 && ff != end3; ff.increment(e3)) {
                const std::string name = ff->path().filename().string();
                const std::string rel = std::string(kFilesDir) + "/" + sha + "/" + name;
                std::error_code e4;
                if (!real_file(*ff) || !valid_basename(name) || named.count(rel)) continue;
                if (fs::remove(ff->path(), e4)) ++report.files_removed;
            }
            fs::remove(fd->path(), e3);  // a real, empty folder only: remove() never empties one
        }
    }
    return report;
}

}  // namespace sonora
