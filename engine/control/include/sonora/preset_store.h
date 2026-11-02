// SPDX-License-Identifier: GPL-3.0-or-later
//
// The user preset store (ADR-0029 §1, §2, §4; slice 4): one folder per
// plugin under $XDG_DATA_HOME/sonora/presets, a canonical-JSON sidecar per
// preset (`sonora_preset` 1), the plugin's own StateBlob content-addressed
// beside it.
//
// Control thread only. Every call here touches the filesystem, allocates
// and may wait on a slow USB stick: nothing in Session::process()'s call
// graph may reach it. A preset reaches the audio side only as a host's
// state, through the make-before-break restart (ADR-0029 §3).
//
// Files in the store are untrusted input: another program, a full disk or a
// power cut may have left any bytes there. A file that does not read as
// ours is reported and skipped, never a crash and never deleted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sonora {

inline constexpr int64_t kPresetFormatVersion = 1;

// Caps on what the store reads or writes. A sidecar holds names and
// parameter values only; 1 MiB is far beyond 4,096 parameters.
inline constexpr std::size_t kMaxPresetSidecarBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxPresetPluginFileBytes = std::size_t{64} << 10;
inline constexpr std::size_t kMaxPresetNameBytes = 256;
inline constexpr std::size_t kMaxPresetParams = 4096;
inline constexpr std::size_t kMaxPresetFiles = 64;
// A file a state names is hashed whole in memory (common/base's SHA-256 is
// one-shot), so it is capped here until a streaming hash exists.
inline constexpr uint64_t kMaxPresetFileBytes = uint64_t{256} << 20;
// Sidecars listed per plugin; a folder with more is listed up to the cap,
// and the list says it was cut.
inline constexpr std::size_t kMaxPresetsPerPlugin = 10'000;

// <user_data_dir()>/presets, or empty when the platform knows no data
// folder for this user.
std::string user_preset_dir();

// The plugin's folder name: the first 128 bits of sha256("<format>:<id>"),
// 32 lowercase hex digits (ADR-0029 §2). Stable across any rename.
std::string preset_plugin_key(std::string_view format, std::string_view id);

// "u-" and 32 lowercase hex digits: the only ids a user preset may have, so
// an id never names a path outside its plugin's folder.
bool is_user_preset_id(std::string_view id);

struct PresetPlugin {
    std::string format;  // "lv2" or "vst3"
    std::string id;
    std::string name;
    std::string vendor;
    std::string version;  // diagnostic only (ADR-0029 §4)
};

struct PresetParam {
    std::string key;
    float value = 0.0f;
};

// A file the state names, relative to the plugin's folder:
// "Plugin Files/<sha256>/<basename>".
struct PresetFileRef {
    std::string path;
    std::string sha256;
    uint64_t bytes = 0;
};

struct PresetSidecar {
    std::string id;
    std::string name;
    PresetPlugin plugin;
    std::vector<PresetParam> params;
    std::string state_file;  // "Plugin State/<sha256>.state"
    std::string state_sha256;
    uint64_t state_bytes = 0;
    std::vector<PresetFileRef> files;
    std::string category;
};

bool encode_preset_sidecar(const PresetSidecar& s, std::string& out, std::string& error);

enum class SidecarRead {
    Ok,
    Newer,      // a `sonora_preset` this build does not know; `error` names it
    Malformed,  // not JSON, a field missing or of the wrong type, a path not ours
};
// Checks every path the sidecar names against the store's layout, so a
// hostile sidecar cannot point a load or a cleanup outside its folder.
SidecarRead decode_preset_sidecar(std::string_view text, PresetSidecar& out, std::string& error);

enum class PresetResult {
    Ok,
    NameTaken,  // another preset of this plugin has the name
    NotFound,
    Invalid,    // the request itself: an empty name, a blob of another plugin, a cap
    Damaged,    // a blob or file fails its sidecar's hash or size, or the sidecar is not ours
    Newer,      // made by a newer Sonora
    IoError,
};

enum class PresetState { Ok, Damaged, Newer };

struct ListedPreset {
    std::string id;  // from the file name when the sidecar cannot be read
    PresetState state = PresetState::Ok;
    std::string why;  // set unless Ok
    // Empty when the sidecar does not read (Newer, or Damaged in itself);
    // kept when only the blob was found damaged, so the menu has its name.
    PresetSidecar sidecar;
};

struct ListedPlugin {
    std::string key;
    PresetPlugin plugin;  // from plugin.json, else from a sidecar; empty if neither reads
    bool plugin_missing = false;  // not installed: listed, kept, loads nothing (§4)
    std::vector<ListedPreset> presets;  // in name order, unreadable ones last
    bool cut = false;  // more than kMaxPresetsPerPlugin sidecars
};

struct PresetFileSource {
    std::string basename;  // what the state calls the file
    // Where it is now. Copied, never linked: the source may be any file the
    // user edits later, and the store's copy must keep its hash.
    std::string source;
};

struct PresetSave {
    PresetPlugin plugin;
    std::string name;
    std::string overwrite_id;  // empty: a new preset
    std::vector<PresetParam> params;
    std::string blob;  // a StateBlob of `plugin`, at most plugin::kMaxStateBytes
    std::vector<PresetFileSource> files;
    std::string category;
};

struct LoadedPreset {
    PresetSidecar sidecar;
    std::string blob;
    std::vector<std::string> file_paths;  // absolute, in sidecar.files order
};

struct PresetCleanup {
    std::size_t blobs_removed = 0;
    std::size_t files_removed = 0;
    std::size_t temporaries_removed = 0;
    // Plugin folders left alone because a sidecar there did not read: what
    // it names cannot be known, so nothing it might name is deleted.
    std::size_t folders_kept = 0;
};

class PresetStore {
public:
    explicit PresetStore(std::string root);

    const std::string& root() const { return root_; }

    // Creates the root and, the first time per object, runs cleanup(). The
    // caller opens the store once per engine run and never while a take is
    // open (ADR-0029 §2).
    bool open(std::string& error);

    // Writes the blob and the files first, each durable, and the sidecar
    // last by rename, so a power cut leaves either the old preset or the new
    // one, and at worst an unnamed blob that cleanup removes.
    PresetResult save(const PresetSave& request, std::string& id_out, std::string& why);
    PresetResult rename(std::string_view format, std::string_view plugin_id, std::string_view preset_id,
                        const std::string& new_name, std::string& why);
    // Removes the sidecar only; its blob goes at the next cleanup when no
    // other sidecar names it.
    PresetResult remove(std::string_view format, std::string_view plugin_id, std::string_view preset_id,
                        std::string& why);
    // Checks the blob's and every file's hash and size against the sidecar,
    // and the blob's identity against the plugin. A failure is Damaged and
    // marks the preset damaged in this object's later lists.
    PresetResult load(std::string_view format, std::string_view plugin_id, std::string_view preset_id,
                      LoadedPreset& out, std::string& why);

    // Reads sidecars only, never a blob (§2).
    ListedPlugin list(std::string_view format, std::string_view plugin_id) const;
    // Every plugin folder, `installed` deciding which are missing.
    std::vector<ListedPlugin> list_all(
        const std::function<bool(const std::string& format, const std::string& id)>& installed) const;

    // Removes blobs and files no readable sidecar names, and temporaries a
    // crashed save left. Never touches a folder holding a sidecar it cannot
    // read.
    PresetCleanup cleanup();

private:
    ListedPlugin list_folder(const std::string& key) const;
    bool no_root(std::string& why) const;

    std::string root_;
    bool cleaned_ = false;
    std::set<std::string> damaged_;  // "<key>/<preset id>", found at load this run
};

}  // namespace sonora
