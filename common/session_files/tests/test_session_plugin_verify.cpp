// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plugin files verified when a session opens (ADR-0151 §2): each file under
// "Plugin Files/<sha256>/" must hash to its folder's digest. A tampered,
// shortened, missing or linked file is refused by its name, and a verifier
// hashes a file once per opened session, so a chain started again during a
// take never reads gigabytes.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/session_files/session_plugin_verify.h"
#include "sonora/sha256.h"

namespace fs = std::filesystem;
using sonora::model::PluginFileRef;
using sonora::session_files::PluginFileVerifier;
using sonora::session_files::plugin_file_on_disk_problem;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-plugin-verify-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

const std::string kSample = "RIFF....WAVEfmt a kick drum sample";

// The file as a host saved it, and the reference the session holds.
PluginFileRef placed(const fs::path& folder, const std::string& name = "Kick.wav") {
    const std::string sha = sonora::sha256_hex(kSample);
    const PluginFileRef f{sonora::model::plugin_files_path(sha, name), sha, kSample.size()};
    write(folder / f.path, kSample);
    return f;
}

bool names(const std::string& why, const std::string& what) {
    return why.find("Kick.wav") != std::string::npos && why.find(what) != std::string::npos;
}

int test_intact_file_verifies() {
    TempDir d;
    const PluginFileRef f = placed(d.path);
    CHECK(plugin_file_on_disk_problem(d.path.string(), f).empty());
    PluginFileVerifier v;
    v.open();
    CHECK(v.problem(d.path.string(), {f}).empty());
    return 0;
}

// Same size, other bytes: only the hash finds it, and it is refused by name.
int test_tampered_file_is_refused_by_name() {
    TempDir d;
    const PluginFileRef f = placed(d.path);
    std::string evil = kSample;
    evil[0] = 'X';
    write(d.path / f.path, evil);
    const std::string why = plugin_file_on_disk_problem(d.path.string(), f);
    CHECK(names(why, "does not match its checksum"));
    PluginFileVerifier v;
    v.open();
    CHECK(names(v.problem(d.path.string(), {f}), "does not match its checksum"));
    return 0;
}

int test_missing_short_and_linked_files() {
    TempDir d;
    const PluginFileRef f = placed(d.path);
    write(d.path / f.path, kSample.substr(1));
    CHECK(names(plugin_file_on_disk_problem(d.path.string(), f), "is not the size"));
    fs::remove(d.path / f.path);
    CHECK(names(plugin_file_on_disk_problem(d.path.string(), f), "is missing"));
    // A link to a good copy elsewhere is still refused: a received folder
    // could point anywhere.
    TempDir other;
    const PluginFileRef g = placed(other.path);
    fs::create_symlink(other.path / g.path, d.path / f.path);
    CHECK(names(plugin_file_on_disk_problem(d.path.string(), f), "behind a link"));
    return 0;
}

// A received session's path is checked before it is joined to the folder.
int test_hostile_path_is_refused() {
    TempDir d;
    PluginFileRef f = placed(d.path);
    f.path = "Plugin Files/" + f.sha256 + "/../../../etc/passwd";
    CHECK(!plugin_file_on_disk_problem(d.path.string(), f).empty());
    return 0;
}

// Hashed once per open: a later tamper that keeps the size is not re-read
// (the open is where it is caught), a later loss of the file is, and a
// failure stands until the next open, without hashing again.
int test_verifier_hashes_once_per_open() {
    TempDir d;
    const PluginFileRef f = placed(d.path);
    PluginFileVerifier v;
    v.open();
    CHECK(v.problem(d.path.string(), {f}).empty());
    std::string evil = kSample;
    evil[1] = 'Y';
    write(d.path / f.path, evil);
    CHECK(v.problem(d.path.string(), {f}).empty());
    v.open();
    CHECK(names(v.problem(d.path.string(), {f}), "does not match its checksum"));
    write(d.path / f.path, kSample);
    CHECK(names(v.problem(d.path.string(), {f}), "does not match its checksum"));
    v.open();
    CHECK(v.problem(d.path.string(), {f}).empty());
    fs::remove(d.path / f.path);
    CHECK(names(v.problem(d.path.string(), {f}), "is missing"));
    return 0;
}

// The first bad file of a list is named, not the good one before it. Files a
// save just made durable are trusted without hashing, but never one that
// failed at open: a save that copied nothing named it without reading it.
int test_trust_and_first_problem() {
    TempDir d;
    const PluginFileRef good = placed(d.path, "Snare.wav");
    const PluginFileRef bad = placed(d.path, "Kick.wav");
    write(d.path / bad.path, std::string(kSample.size(), 'Z'));
    PluginFileVerifier v;
    v.open();
    const std::string why = v.problem(d.path.string(), {good, bad});
    CHECK(names(why, "does not match") && why.find("Snare.wav") == std::string::npos);
    v.trust({bad});
    CHECK(names(v.problem(d.path.string(), {bad}), "does not match"));

    TempDir e;
    const PluginFileRef saved = placed(e.path);
    v.open();
    v.trust({saved});
    write(e.path / saved.path, std::string(kSample.size(), 'Q'));  // same size: only a hash would see it
    CHECK(v.problem(e.path.string(), {saved}).empty());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_intact_file_verifies();
    failed += test_tampered_file_is_refused_by_name();
    failed += test_missing_short_and_linked_files();
    failed += test_hostile_path_is_refused();
    failed += test_verifier_hashes_once_per_open();
    failed += test_trust_and_first_problem();
    std::printf("%s\n", failed == 0 ? "session_plugin_verify: all passed" : "session_plugin_verify: FAILED");
    return failed == 0 ? 0 : 1;
}
