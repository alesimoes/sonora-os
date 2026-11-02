// SPDX-License-Identifier: GPL-3.0-or-later
//
// The user preset store (ADR-0029 slice 4): the layout of §2, names unique
// per plugin, the blob and files checked at load and never at list, a
// sidecar of a newer Sonora listed and not loaded, shared blobs kept by
// cleanup, a missing plugin's presets kept, and a store left by a crash or
// by another program read without a crash and without losing a preset.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "sonora/plugin/host_control.h"
#include "sonora/preset_store.h"
#include "sonora/sha256.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
namespace pc = sonora::plugin::control;
using namespace sonora;

const char* const kDpl = "http://gareus.org/oss/lv2/dpl#mono";

fs::path fresh(const char* what) {
    const fs::path p = fs::temp_directory_path() / (std::string("sonora_test_preset_store_") + what);
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

std::string blob_of(const std::string& id, const std::string& body,
                    pc::Format format = pc::Format::Lv2) {
    pc::StateBlob b;
    b.format = format;
    b.id = id;
    b.sections.push_back(body);
    std::string out, e;
    pc::encode_state_blob(b, out, e);
    return out;
}

PresetPlugin dpl() { return {"lv2", kDpl, "x42-dpl Digital Peak Limiter Mono", "x42", "0.9.2"}; }

PresetSave save_of(const std::string& name, const std::string& body = "threshold state") {
    PresetSave s;
    s.plugin = dpl();
    s.name = name;
    s.params = {{"threshold", -1.0f}, {"release", 0.125f}};
    s.blob = blob_of(kDpl, body);
    return s;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << bytes;
}

std::size_t count_files(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) ++n;
    return n;
}

const ListedPreset* find(const ListedPlugin& l, const std::string& id) {
    for (const ListedPreset& p : l.presets)
        if (p.id == id) return &p;
    return nullptr;
}

int store_dir_follows_xdg() {
    const char* old_xdg = std::getenv("XDG_DATA_HOME");
    const std::string keep = old_xdg ? old_xdg : "";
    const char* old_home = std::getenv("HOME");
    const std::string keep_home = old_home ? old_home : "";
    ::setenv("XDG_DATA_HOME", "/srv/data", 1);
    CHECK(user_preset_dir() == "/srv/data/sonora/presets");
    // A relative XDG_DATA_HOME is invalid by the XDG rule and ignored.
    ::setenv("XDG_DATA_HOME", "relative", 1);
    ::setenv("HOME", "/home/u", 1);
    CHECK(user_preset_dir() == "/home/u/.local/share/sonora/presets");
    if (old_xdg) ::setenv("XDG_DATA_HOME", keep.c_str(), 1);
    else ::unsetenv("XDG_DATA_HOME");
    if (old_home) ::setenv("HOME", keep_home.c_str(), 1);
    return 0;
}

int layout_and_round_trip() {
    const fs::path root = fresh("layout");
    PresetStore store(root.string());
    std::string why, id;
    CHECK(store.open(why));
    const PresetSave req = save_of("Lead Vocal Bright");
    CHECK(store.save(req, id, why) == PresetResult::Ok);
    CHECK(is_user_preset_id(id));

    const std::string key = preset_plugin_key("lv2", kDpl);
    CHECK(key == sha256_hex(std::string("lv2:") + kDpl).substr(0, 32));
    const fs::path dir = root / key;
    const std::string sha = sha256_hex(req.blob);
    CHECK(fs::is_regular_file(dir / "plugin.json"));
    CHECK(fs::is_regular_file(dir / (id + ".preset.json")));
    CHECK(read_file(dir / "Plugin State" / (sha + ".state")) == req.blob);
    // The preset's name is in the sidecar only, never in a file name.
    for (const auto& e : fs::recursive_directory_iterator(dir))
        CHECK(e.path().filename().string().find("Lead") == std::string::npos);
    // Nothing temporary is left.
    CHECK(count_files(root) == 3);

    PresetSidecar side;
    CHECK(decode_preset_sidecar(read_file(dir / (id + ".preset.json")), side, why) == SidecarRead::Ok);
    CHECK(side.name == "Lead Vocal Bright" && side.plugin.version == "0.9.2");
    CHECK(side.state_file == "Plugin State/" + sha + ".state" && side.state_bytes == req.blob.size());

    const ListedPlugin l = store.list("lv2", kDpl);
    CHECK(l.presets.size() == 1 && l.presets[0].id == id && l.presets[0].state == PresetState::Ok);
    CHECK(l.presets[0].sidecar.name == "Lead Vocal Bright");
    CHECK(l.plugin.name == "x42-dpl Digital Peak Limiter Mono");

    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Ok);
    CHECK(got.blob == req.blob);
    CHECK(got.sidecar.params.size() == 2 && got.sidecar.params[0].key == "threshold" &&
          got.sidecar.params[0].value == -1.0f && got.sidecar.params[1].value == 0.125f);
    // Another plugin's request finds nothing under this id.
    CHECK(store.load("lv2", "urn:other", id, got, why) == PresetResult::NotFound);
    CHECK(store.load("vst3", kDpl, id, got, why) == PresetResult::NotFound);
    fs::remove_all(root);
    return 0;
}

int names_unique_per_plugin() {
    const fs::path root = fresh("names");
    PresetStore store(root.string());
    std::string why, a, b, c;
    CHECK(store.open(why));
    CHECK(store.save(save_of("Bright"), a, why) == PresetResult::Ok);
    std::string dup;
    CHECK(store.save(save_of("Bright", "other"), dup, why) == PresetResult::NameTaken);
    CHECK(!why.empty());
    // Case-sensitive (Q6): both may exist.
    CHECK(store.save(save_of("bright"), b, why) == PresetResult::Ok);
    // Overwrite names the existing preset's id.
    PresetSave over = save_of("Bright", "brighter");
    over.overwrite_id = a;
    std::string same;
    CHECK(store.save(over, same, why) == PresetResult::Ok && same == a);
    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, a, got, why) == PresetResult::Ok && got.blob == over.blob);
    // Overwriting one preset under another's name is still a collision.
    over.overwrite_id = b;
    CHECK(store.save(over, same, why) == PresetResult::NameTaken);
    over.overwrite_id = "u-00000000000000000000000000000000";
    CHECK(store.save(over, same, why) == PresetResult::NotFound);
    // Another plugin's preset may have the name.
    PresetSave other = save_of("Bright");
    other.plugin.id = "urn:other";
    other.blob = blob_of("urn:other", "x");
    CHECK(store.save(other, c, why) == PresetResult::Ok);

    CHECK(store.rename("lv2", kDpl, b, "Bright", why) == PresetResult::NameTaken);
    CHECK(store.rename("lv2", kDpl, b, "Dark", why) == PresetResult::Ok);
    CHECK(store.rename("lv2", kDpl, b, "Dark", why) == PresetResult::Ok);
    CHECK(store.load("lv2", kDpl, b, got, why) == PresetResult::Ok && got.sidecar.name == "Dark");
    CHECK(store.rename("lv2", kDpl, "u-11111111111111111111111111111111", "X", why) == PresetResult::NotFound);

    // Names a sidecar or a menu cannot hold are refused.
    for (const std::string& bad : {std::string(), std::string("  "), std::string("a\nb"),
                                  std::string("\xff\xfe"), std::string(kMaxPresetNameBytes + 1, 'n')}) {
        std::string id;
        CHECK(store.save(save_of(bad), id, why) == PresetResult::Invalid);
        CHECK(store.rename("lv2", kDpl, a, bad, why) == PresetResult::Invalid);
    }
    CHECK(store.list("lv2", kDpl).presets.size() == 2);
    fs::remove_all(root);
    return 0;
}

int requests_checked() {
    const fs::path root = fresh("requests");
    PresetStore store(root.string());
    std::string why, id;
    CHECK(store.open(why));
    PresetSave s = save_of("Wrong");
    s.blob = blob_of("urn:someone-else", "x");
    CHECK(store.save(s, id, why) == PresetResult::Invalid);
    s.blob = blob_of(kDpl, "x", pc::Format::Vst3);
    CHECK(store.save(s, id, why) == PresetResult::Invalid);
    s.blob = "not a state blob";
    CHECK(store.save(s, id, why) == PresetResult::Invalid);
    s = save_of("Params");
    s.params.assign(kMaxPresetParams + 1, PresetParam{"k", 0.0f});
    CHECK(store.save(s, id, why) == PresetResult::Invalid);
    s = save_of("Format");
    s.plugin.format = "vst2";
    CHECK(store.save(s, id, why) == PresetResult::Invalid);
    s = save_of("Basename");
    for (const char* bad : {"../escape.wav", "a/b.wav", "..", ".", ""}) {
        s.files = {{bad, (root / "nowhere").string()}};
        CHECK(store.save(s, id, why) == PresetResult::Invalid);
    }
    // Hostile ids never become paths.
    LoadedPreset got;
    for (const char* bad : {"../../etc/passwd", "u-../../x", "u-ABCDEF0123456789ABCDEF0123456789", ""}) {
        CHECK(!is_user_preset_id(bad));
        CHECK(store.load("lv2", kDpl, bad, got, why) == PresetResult::NotFound);
        CHECK(store.remove("lv2", kDpl, bad, why) == PresetResult::NotFound);
    }
    CHECK(!fs::exists(root / preset_plugin_key("lv2", kDpl)) ||
          store.list("lv2", kDpl).presets.empty());
    fs::remove_all(root);
    return 0;
}

int damaged_blob_refused_at_load() {
    const fs::path root = fresh("damaged");
    PresetStore store(root.string());
    std::string why, id;
    CHECK(store.open(why));
    const PresetSave req = save_of("Fragile");
    CHECK(store.save(req, id, why) == PresetResult::Ok);
    const fs::path blob =
        root / preset_plugin_key("lv2", kDpl) / "Plugin State" / (sha256_hex(req.blob) + ".state");
    std::string bytes = read_file(blob);
    bytes[bytes.size() / 2] ^= 0x5a;
    write_file(blob, bytes);
    // Listing reads sidecars only: still Ok until a load finds it.
    CHECK(store.list("lv2", kDpl).presets[0].state == PresetState::Ok);
    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Damaged);
    CHECK(why.find("damaged") != std::string::npos);
    CHECK(got.blob.empty());
    CHECK(store.list("lv2", kDpl).presets[0].state == PresetState::Damaged);
    // Truncated, and gone.
    write_file(blob, bytes.substr(0, 4));
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Damaged);
    fs::remove(blob);
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Damaged);
    // Nothing deleted the sidecar: a damaged preset is the user's to remove.
    PresetStore again(root.string());
    CHECK(again.open(why));
    CHECK(again.list("lv2", kDpl).presets.size() == 1);
    CHECK(again.remove("lv2", kDpl, id, why) == PresetResult::Ok);
    fs::remove_all(root);
    return 0;
}

int files_kept_with_the_preset() {
    const fs::path root = fresh("files");
    const fs::path src = fresh("files_src");
    write_file(src / "kick.wav", std::string(4096, 'k'));
    PresetStore store(root.string());
    std::string why, id;
    CHECK(store.open(why));
    PresetSave req = save_of("Kit");
    req.files = {{"kick.wav", (src / "kick.wav").string()}};
    CHECK(store.save(req, id, why) == PresetResult::Ok);
    const std::string sha = sha256_hex(std::string(4096, 'k'));
    const fs::path stored = root / preset_plugin_key("lv2", kDpl) / "Plugin Files" / sha / "kick.wav";
    CHECK(read_file(stored) == std::string(4096, 'k'));
    // The store holds its own copy: the source may go.
    fs::remove_all(src);
    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Ok);
    CHECK(got.sidecar.files.size() == 1 && got.sidecar.files[0].sha256 == sha &&
          got.sidecar.files[0].bytes == 4096);
    CHECK(got.file_paths.size() == 1 && fs::path(got.file_paths[0]) == stored);
    write_file(stored, std::string(4096, 'x'));
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Damaged);
    fs::remove_all(root);
    return 0;
}

int failed_save_leaves_no_half_preset() {
    const fs::path root = fresh("atomic");
    PresetStore store(root.string());
    std::string why, id;
    CHECK(store.open(why));
    // A blob folder that cannot be made: the save fails before any sidecar.
    write_file(root / preset_plugin_key("lv2", kDpl) / "Plugin State", "in the way");
    CHECK(store.save(save_of("Blocked"), id, why) == PresetResult::IoError);
    CHECK(!why.empty());
    CHECK(store.list("lv2", kDpl).presets.empty());
    fs::remove(root / preset_plugin_key("lv2", kDpl) / "Plugin State");

    // An overwrite whose file cannot be read leaves the old preset whole.
    const PresetSave first = save_of("Keep");
    CHECK(store.save(first, id, why) == PresetResult::Ok);
    PresetSave over = save_of("Keep", "new state");
    over.overwrite_id = id;
    over.files = {{"gone.wav", (root / "no such file.wav").string()}};
    std::string same;
    CHECK(store.save(over, same, why) == PresetResult::IoError);
    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Ok && got.blob == first.blob);
    CHECK(store.list("lv2", kDpl).presets.size() == 1);

    // A crash mid-write leaves a temporary: never listed, removed by the
    // next run's cleanup, the real sidecar untouched.
    const fs::path dir = root / preset_plugin_key("lv2", kDpl);
    write_file(dir / (id + ".preset.json.tmp-123-456"), "{\"sonora_preset\": 1, \"na");
    write_file(dir / "Plugin State" / (std::string(64, '9') + ".state.tmp-1-2"), "half");
    CHECK(store.list("lv2", kDpl).presets.size() == 1);
    PresetStore next(root.string());
    CHECK(next.open(why));
    CHECK(!fs::exists(dir / (id + ".preset.json.tmp-123-456")));
    CHECK(!fs::exists(dir / "Plugin State" / (std::string(64, '9') + ".state.tmp-1-2")));
    CHECK(next.load("lv2", kDpl, id, got, why) == PresetResult::Ok);
    fs::remove_all(root);
    return 0;
}

int newer_sidecar_listed_not_loaded() {
    const fs::path root = fresh("newer");
    PresetStore store(root.string());
    std::string why, id;
    CHECK(store.open(why));
    CHECK(store.save(save_of("Today"), id, why) == PresetResult::Ok);
    const fs::path dir = root / preset_plugin_key("lv2", kDpl);
    const std::string future = "u-0123456789abcdef0123456789abcdef";
    write_file(dir / (future + ".preset.json"),
               "{\"sonora_preset\": 2, \"id\": \"" + future + "\", \"name\": \"Tomorrow\", \"wavelets\": []}\n");
    const ListedPlugin l = store.list("lv2", kDpl);
    CHECK(l.presets.size() == 2);
    const ListedPreset* f = find(l, future);
    CHECK(f && f->state == PresetState::Newer && f->why.find("2") != std::string::npos);
    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, future, got, why) == PresetResult::Newer);
    // Its name is not known, so it blocks no save; and nothing removes it.
    CHECK(store.rename("lv2", kDpl, future, "Mine", why) == PresetResult::Newer);
    PresetStore next(root.string());
    CHECK(next.open(why));
    CHECK(fs::exists(dir / (future + ".preset.json")));
    fs::remove_all(root);
    return 0;
}

int hostile_files_reported_and_skipped() {
    const fs::path root = fresh("hostile");
    PresetStore store(root.string());
    std::string why, good;
    CHECK(store.open(why));
    CHECK(store.save(save_of("Good"), good, why) == PresetResult::Ok);
    const fs::path dir = root / preset_plugin_key("lv2", kDpl);
    const std::string side = read_file(dir / (good + ".preset.json"));
    const std::string sha = sha256_hex(save_of("Good").blob);

    auto replace = [](std::string s, const std::string& from, const std::string& to) {
        const auto at = s.find(from);
        if (at != std::string::npos) s.replace(at, from.size(), to);
        return s;
    };
    const std::string hostile[] = {
        side.substr(0, side.size() / 2),                     // truncated
        "",                                                  // empty
        "\xef\xbb\xbf" + side,                               // a byte-order mark
        "[1, 2, 3]",                                          // not an object
        std::string(200, '[') + std::string(200, ']'),      // deeper than the cap
        replace(side, "Plugin State/" + sha, "../../../../etc/" + sha),  // outside the folder
        replace(side, "\"sha256\": \"" + sha, "\"sha256\": \"zz"),        // not a hash
        replace(side, "\"lv2\"", "\"vst3\""),                // another plugin's folder
        replace(side, "\"Good\"", "\"\xc0\xaf\""),           // overlong UTF-8
        replace(side, "\"bytes\": ", "\"bytes\": -"),          // a negative size
    };
    int n = 0;
    for (const std::string& h : hostile) {
        char name[64];
        std::snprintf(name, sizeof name, "u-%032x.preset.json", 0xbad0 + n++);
        char id[40];
        std::snprintf(id, sizeof id, "u-%032x", 0xbad0 + n - 1);
        // Its own id inside, so only the one fault makes it damaged.
        write_file(dir / name, replace(h, good, id));
    }
    // Over the cap: refused before it is read.
    write_file(dir / "u-ffffffffffffffffffffffffffffffff.preset.json",
               std::string(kMaxPresetSidecarBytes + 1, ' '));
    // A folder with a sidecar's name, and a file that is not ours.
    fs::create_directories(dir / "u-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.preset.json");
    write_file(dir / "notes.txt", "mine");
    write_file(dir / "plugin.json", "{\"format\": ");

    const ListedPlugin l = store.list("lv2", kDpl);
    std::size_t damaged = 0;
    for (const ListedPreset& p : l.presets) {
        if (p.id == good) {
            CHECK(p.state == PresetState::Ok);
        } else {
            CHECK(p.state == PresetState::Damaged && !p.why.empty());
            ++damaged;
        }
    }
    CHECK(damaged == std::size_t(n) + 2);
    // The good one is still listed by its name although plugin.json is
    // broken: its sidecar names the plugin.
    CHECK(l.plugin.id == kDpl);
    LoadedPreset got;
    CHECK(store.load("lv2", kDpl, good, got, why) == PresetResult::Ok);
    for (int i = 0; i < n; ++i) {
        char id[40];
        std::snprintf(id, sizeof id, "u-%032x", 0xbad0 + i);
        CHECK(store.load("lv2", kDpl, id, got, why) == PresetResult::Damaged);
    }
    // A damaged sidecar blocks no name, and a save beside it works.
    std::string another;
    CHECK(store.save(save_of("Good too"), another, why) == PresetResult::Ok);
    // Cleanup leaves this folder alone: the broken sidecars may name blobs.
    write_file(dir / "Plugin State" / (std::string(64, 'a') + ".state"), "orphan?");
    PresetStore next(root.string());
    CHECK(next.open(why));
    CHECK(fs::exists(dir / "Plugin State" / (std::string(64, 'a') + ".state")));
    CHECK(fs::exists(dir / "notes.txt"));
    // A plugin folder holding junk at the root of the store is skipped too.
    write_file(root / "README", "x");
    write_file(root / "not-a-key" / "u-00000000000000000000000000000000.preset.json", side);
    const auto all = next.list_all([](const std::string&, const std::string&) { return true; });
    CHECK(all.size() == 1);
    fs::remove_all(root);
    return 0;
}

int shared_blob_kept_by_cleanup() {
    const fs::path root = fresh("shared");
    std::string why, a, b;
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        CHECK(store.save(save_of("One", "same"), a, why) == PresetResult::Ok);
        CHECK(store.save(save_of("Two", "same"), b, why) == PresetResult::Ok);
        const fs::path states = root / preset_plugin_key("lv2", kDpl) / "Plugin State";
        CHECK(count_files(states) == 1);
        CHECK(store.remove("lv2", kDpl, a, why) == PresetResult::Ok);
        CHECK(store.remove("lv2", kDpl, a, why) == PresetResult::NotFound);
    }
    const fs::path states = root / preset_plugin_key("lv2", kDpl) / "Plugin State";
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        CHECK(count_files(states) == 1);
        LoadedPreset got;
        CHECK(store.load("lv2", kDpl, b, got, why) == PresetResult::Ok);
        CHECK(store.remove("lv2", kDpl, b, why) == PresetResult::Ok);
        // Only the first open per object cleans.
        CHECK(store.open(why));
        CHECK(count_files(states) == 1);
    }
    PresetStore store(root.string());
    CHECK(store.open(why));
    CHECK(count_files(states) == 0);
    fs::remove_all(root);
    return 0;
}

int missing_plugin_presets_kept() {
    const fs::path root = fresh("missing");
    std::string why, a, b;
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        CHECK(store.save(save_of("Gone"), a, why) == PresetResult::Ok);
        // The plugin renamed in its new version: same folder, plugin.json
        // follows the name last seen.
        PresetSave renamed = save_of("Renamed");
        renamed.plugin.name = "x42 Limiter";
        CHECK(store.save(renamed, b, why) == PresetResult::Ok);
        CHECK(store.list("lv2", kDpl).presets.size() == 2);
        CHECK(store.list("lv2", kDpl).plugin.name == "x42 Limiter");
    }
    PresetStore store(root.string());
    CHECK(store.open(why));
    const auto all = store.list_all([](const std::string&, const std::string&) { return false; });
    CHECK(all.size() == 1);
    CHECK(all[0].plugin_missing && all[0].plugin.id == kDpl && all[0].plugin.name == "x42 Limiter");
    CHECK(all[0].presets.size() == 2);
    CHECK(all[0].presets[0].sidecar.name == "Gone" && all[0].presets[1].sidecar.name == "Renamed");
    CHECK(count_files(root / preset_plugin_key("lv2", kDpl) / "Plugin State") == 1);
    const auto present = store.list_all([](const std::string& f, const std::string& id) {
        return f == "lv2" && id == kDpl;
    });
    CHECK(present.size() == 1 && !present[0].plugin_missing);
    fs::remove_all(root);
    return 0;
}

int sidecar_codec() {
    PresetSidecar s;
    s.id = "u-7f3a2c9e4b1d4e0f8a6b5c4d3e2f1a0b";
    s.name = "Lead \xc3\xa9 \"quoted\"";
    s.plugin = dpl();
    s.params = {{"threshold", -1.0f}, {"tiny", 1e-30f}};
    s.state_sha256 = std::string(64, 'c');
    s.state_file = "Plugin State/" + s.state_sha256 + ".state";
    s.state_bytes = 812;
    s.files = {{"Plugin Files/" + std::string(64, 'd') + "/kick.wav", std::string(64, 'd'), 482144}};
    std::string text, why;
    CHECK(encode_preset_sidecar(s, text, why));
    CHECK(text.find("\"sonora_preset\": 1") != std::string::npos);
    PresetSidecar back;
    CHECK(decode_preset_sidecar(text, back, why) == SidecarRead::Ok);
    CHECK(back.name == s.name && back.params.size() == 2 && back.params[1].value == 1e-30f);
    CHECK(back.files.size() == 1 && back.files[0].bytes == 482144);
    std::string again;
    CHECK(encode_preset_sidecar(back, again, why) && again == text);
    // A file path that does not match its hash is not ours.
    back.files[0].path = "Plugin Files/" + std::string(64, 'e') + "/kick.wav";
    CHECK(encode_preset_sidecar(back, again, why));
    CHECK(decode_preset_sidecar(again, back, why) == SidecarRead::Malformed);
    return 0;
}

// #275 review B1: cleanup removed every file whose name held ".tmp-", and a
// state could name such a file, so the next open deleted the user's sample.
int temp_shaped_names_kept() {
    const fs::path root = fresh("tempnames");
    const fs::path src = fresh("tempnames_src");
    const std::string kick(2048, 'k');
    write_file(src / "kick.tmp-1.wav", kick);
    write_file(src / "kick.wav", kick);
    const std::string sha = sha256_hex(kick);
    std::string why, id, plain;
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        PresetSave req = save_of("Temp named");
        req.files = {{"kick.tmp-1.wav", (src / "kick.tmp-1.wav").string()}};
        CHECK(store.save(req, id, why) == PresetResult::Invalid);
        req = save_of("Plain");
        req.files = {{"kick.wav", (src / "kick.wav").string()}};
        CHECK(store.save(req, plain, why) == PresetResult::Ok);
    }
    const fs::path dir = root / preset_plugin_key("lv2", kDpl);
    // Only the exact shape a save leaves is a temporary; anything else that
    // merely holds the mark is someone's file.
    write_file(dir / "notes.tmp-final", "mine");
    write_file(dir / "Plugin State" / "readme.tmp-1-2", "mine");
    write_file(dir / "Plugin State" / (std::string(64, 'a') + ".state.tmp-7-8"), "half");
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        CHECK(fs::exists(dir / "notes.tmp-final"));
        CHECK(fs::exists(dir / "Plugin State" / "readme.tmp-1-2"));
        CHECK(!fs::exists(dir / "Plugin State" / (std::string(64, 'a') + ".state.tmp-7-8")));
        LoadedPreset got;
        CHECK(store.load("lv2", kDpl, plain, got, why) == PresetResult::Ok);
    }
    // A sidecar naming such a file (written by hand, or by an older build):
    // not loaded, and the file survives every reopen.
    const fs::path side = dir / (plain + ".preset.json");
    std::string text = read_file(side);
    const std::string from = "/kick.wav\"";
    CHECK(text.find(from) != std::string::npos);
    text.replace(text.find(from), from.size(), "/kick.tmp-1.wav\"");
    write_file(side, text);
    const fs::path forged = dir / "Plugin Files" / sha / "kick.tmp-1.wav";
    write_file(forged, kick);
    for (int i = 0; i < 2; ++i) {
        PresetStore store(root.string());
        CHECK(store.open(why));
        CHECK(fs::exists(forged) && read_file(forged) == kick);
    }
    fs::remove_all(root);
    fs::remove_all(src);
    return 0;
}

// #275 review B2: cleanup followed symlinks, so a link inside the store
// emptied the folder it pointed at.
int symlinks_never_followed() {
    const fs::path root = fresh("symlinks");
    const fs::path outside = fresh("symlinks_outside");
    const std::string blob_name = std::string(64, 'b') + ".state";
    write_file(outside / "state" / blob_name, "keep");
    write_file(outside / "files" / "a.wav", "keep");
    write_file(outside / "files" / (std::string(64, 'c') + ".state"), "keep");
    write_file(outside / "files" / "x.wav.tmp-1-2", "keep");
    write_file(outside / "folder" / "Plugin State" / (std::string(64, 'd') + ".state"), "keep");
    write_file(outside / "folder" / "Plugin State" / "y.state.tmp-1-2", "keep");
    std::string why, a, b;
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        CHECK(store.save(save_of("A"), a, why) == PresetResult::Ok);
        PresetSave other = save_of("B");
        other.plugin.id = "urn:other";
        other.blob = blob_of("urn:other", "b");
        CHECK(store.save(other, b, why) == PresetResult::Ok);
    }
    const fs::path dir_a = root / preset_plugin_key("lv2", kDpl);
    const fs::path dir_b = root / preset_plugin_key("lv2", "urn:other");
    fs::create_directories(dir_a / "Plugin Files");
    fs::create_directory_symlink(outside / "files", dir_a / "Plugin Files" / std::string(64, 'e'));
    fs::remove_all(dir_b / "Plugin State");
    fs::create_directory_symlink(outside / "state", dir_b / "Plugin State");
    fs::create_directory_symlink(outside / "folder", root / std::string(32, 'f'));
    {
        PresetStore store(root.string());
        CHECK(store.open(why));
        // Nor is a save written through the link.
        PresetSave other = save_of("B2");
        other.plugin.id = "urn:other";
        other.blob = blob_of("urn:other", "b2");
        std::string id;
        CHECK(store.save(other, id, why) == PresetResult::IoError);
        CHECK(count_files(outside / "state") == 1);
    }
    CHECK(read_file(outside / "state" / blob_name) == "keep");
    CHECK(read_file(outside / "files" / "a.wav") == "keep");
    CHECK(read_file(outside / "files" / (std::string(64, 'c') + ".state")) == "keep");
    CHECK(read_file(outside / "files" / "x.wav.tmp-1-2") == "keep");
    CHECK(read_file(outside / "folder" / "Plugin State" / (std::string(64, 'd') + ".state")) == "keep");
    CHECK(read_file(outside / "folder" / "Plugin State" / "y.state.tmp-1-2") == "keep");
    std::error_code ec;
    CHECK(fs::is_symlink(fs::symlink_status(dir_a / "Plugin Files" / std::string(64, 'e'), ec)));
    CHECK(fs::is_symlink(fs::symlink_status(dir_b / "Plugin State", ec)));
    fs::remove_all(root);
    fs::remove_all(outside);
    return 0;
}

// #275 review: a store whose root is empty (open() skipped or failed) must
// not resolve its paths against the working directory.
int empty_root_refused() {
    std::error_code gone;
    fs::remove_all(fs::current_path() / preset_plugin_key("lv2", kDpl), gone);
    PresetStore store("");
    std::string why, id;
    CHECK(store.save(save_of("Nowhere"), id, why) == PresetResult::IoError && !why.empty());
    LoadedPreset got;
    const std::string some = "u-0123456789abcdef0123456789abcdef";
    CHECK(store.load("lv2", kDpl, some, got, why) == PresetResult::IoError);
    CHECK(store.rename("lv2", kDpl, some, "X", why) == PresetResult::IoError);
    CHECK(store.remove("lv2", kDpl, some, why) == PresetResult::IoError);
    CHECK(store.list("lv2", kDpl).presets.empty());
    const bool in_cwd = fs::exists(fs::current_path() / preset_plugin_key("lv2", kDpl));
    std::error_code ec;
    fs::remove_all(fs::current_path() / preset_plugin_key("lv2", kDpl), ec);
    CHECK(!in_cwd);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += store_dir_follows_xdg();
    failed += sidecar_codec();
    failed += layout_and_round_trip();
    failed += names_unique_per_plugin();
    failed += requests_checked();
    failed += damaged_blob_refused_at_load();
    failed += files_kept_with_the_preset();
    failed += failed_save_leaves_no_half_preset();
    failed += newer_sidecar_listed_not_loaded();
    failed += hostile_files_reported_and_skipped();
    failed += shared_blob_kept_by_cleanup();
    failed += missing_plugin_presets_kept();
    failed += temp_shaped_names_kept();
    failed += symlinks_never_followed();
    failed += empty_root_refused();
    if (failed == 0) std::printf("preset_store: all tests passed\n");
    return failed == 0 ? 0 : 1;
}
