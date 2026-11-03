// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plugin files in the session (ADR-0029 §6 and slice 2): the files an LV2
// state names live in "<session>/Plugin Files/<sha256>/<basename>". A save
// places each one the session file names in the file's own folder before
// the file names it (from the engine's folder at the untitled rename and at
// Save As), syncs "Plugin Files/" and each "<sha256>/", and never follows a
// symlink out of a received folder: a link anywhere on the way refuses the
// save rather than carry what it points at into the new folder.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/sha256.h"

using namespace model_test;
using sonora::session_files::SaveAsReport;
using sonora::session_files::SessionFolder;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-plugin-files-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

const std::string kBlob = "a sampler's state blob";
const std::string kSample = "RIFF....WAVEfmt a kick drum sample";

// An audio track with one insert whose state names one plugin file, as the
// host's State reply hands it over.
struct Sampler {
    History h{new_session(settings())};
    std::string sha = sonora::sha256_hex(kBlob);
    PluginFileRef file;
    Sampler() {
        Ids ids(h.current());
        const AddTrack t = add_track(ids, "Kick");
        std::string why;
        h.apply(t, why);
        AddInsert in;
        in.track = t.track;
        in.insert = ids();
        in.slot = 0;
        in.plugin = PluginRef{PluginFormat::Lv2, "urn:sonora:test#files", "files", "test"};
        h.apply(in, why);
        const std::string fsha = sonora::sha256_hex(kSample);
        file = PluginFileRef{plugin_files_path(fsha, "Kick.wav"), fsha, kSample.size()};
        StateRef ref{plugin_state_file(sha), sha, kBlob.size(), {file}};
        if (!h.apply(SetInsertState{in.insert, ref}, why)) std::fprintf(stderr, "  %s\n", why.c_str());
    }
    std::string bytes() const {
        std::string out, error;
        serialize(h.current(), WriteInfo{"test", 1}, out, error);
        return out;
    }
    // What the host leaves in `folder` after a GetState with that save_dir,
    // and the engine after the flush.
    void flushed_into(SessionFolder& f, const fs::path& folder) const {
        std::string why;
        if (!f.write_states(folder.string(), {{sha, kBlob}}, why)) std::fprintf(stderr, "  %s\n", why.c_str());
        write(folder / file.path, kSample);
    }
};

std::set<std::string> g_synced;
bool recording_sync(const std::string& path) {
    g_synced.insert(fs::path(path).lexically_normal().string());
    return true;
}

// A state whose file was copied by the host into the folder it saves: the
// save names it and makes "Plugin Files/" and "<sha256>/" durable.
int test_in_place_is_synced() {
    TempDir tmp;
    const Sampler s;
    CHECK(mentions(s.bytes(), "Plugin Files/"));
    g_synced.clear();
    SessionFolder f(recording_sync);
    const fs::path folder = tmp.path / "Mix";
    s.flushed_into(f, folder);
    std::string why, caution;
    CHECK(f.save((folder / "Mix.sonora").string(), s.bytes(), s.h.current(), why, caution, folder.string()));
    CHECK(caution.empty());
    CHECK(read(folder / s.file.path) == kSample);
    CHECK(g_synced.count((folder / "Plugin Files").string()) == 1);
    CHECK(g_synced.count((folder / "Plugin Files" / s.file.sha256).string()) == 1);
    return 0;
}

// The untitled rename and Save As: the files are in the engine's folder,
// and the save puts them in the new one; a copy of that folder anywhere
// else reopens with them.
int test_files_follow_the_session_file() {
    TempDir tmp;
    const Sampler s;
    SessionFolder f;
    const fs::path engine_dir = tmp.path / "Untitled";
    s.flushed_into(f, engine_dir);
    std::string why, caution;
    const fs::path file = tmp.path / "Mix" / "Mix.sonora";
    if (!f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()))
        std::fprintf(stderr, "  %s\n", why.c_str());
    CHECK(f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()));
    CHECK(read(tmp.path / "Mix" / s.file.path) == kSample);

    SaveAsReport rep;
    const fs::path other = tmp.path / "Other" / "Take 2" / "Take 2.sonora";
    const bool saved_as =
        f.save_as(other.string(), s.bytes(), s.h.current(), (tmp.path / "Mix").string(), true, why, caution, &rep);
    if (!saved_as) std::fprintf(stderr, "  %s\n", why.c_str());
    CHECK(saved_as);
    CHECK(f.plugin_files_placed() == 2);
    CHECK(read(other.parent_path() / s.file.path) == kSample);
    // The old folder is as it was.
    CHECK(read(tmp.path / "Mix" / s.file.path) == kSample);

    const fs::path stick = tmp.path / "Stick" / "Take 2";
    fs::create_directories(stick.parent_path());
    fs::copy(other.parent_path(), stick, fs::copy_options::recursive);
    fs::remove_all(engine_dir);
    fs::remove_all(tmp.path / "Mix");
    Loaded loaded;
    LoadError error;
    CHECK(deserialize(read(stick / "Take 2.sonora"), loaded, error));
    const Snapshot& snap = loaded.snapshot;
    const InsertMix* m = snap.mix.find_insert(snap.edit->tracks[0]->inserts[0].id);
    CHECK(m != nullptr && m->state && m->state->files.size() == 1);
    CHECK(sonora::sha256_hex(read(stick / m->state->files[0].path)) == m->state->files[0].sha256);
    return 0;
}

// A received folder is untrusted: "Plugin Files", "<sha256>" or the file
// itself a symlink to something outside refuses Save As, and what the link
// points at is never copied into the new folder.
int test_links_are_never_followed() {
    for (int where = 0; where < 3; ++where) {
        TempDir tmp;
        const Sampler s;
        SessionFolder f;
        const fs::path src = tmp.path / "Received";
        std::string w;
        f.write_states(src.string(), {{s.sha, kBlob}}, w);
        write(tmp.path / "secret" / s.file.sha256 / "Kick.wav", kSample);
        const fs::path files = src / "Plugin Files";
        fs::create_directories(files);
        if (where == 0) {
            fs::create_directory_symlink(tmp.path / "secret", files.parent_path() / "Plugin Files.tmp");
            fs::remove(files);
            fs::rename(files.parent_path() / "Plugin Files.tmp", files);
        } else if (where == 1) {
            fs::create_directory_symlink(tmp.path / "secret" / s.file.sha256, files / s.file.sha256);
        } else {
            fs::create_directories(files / s.file.sha256);
            fs::create_symlink(tmp.path / "secret" / s.file.sha256 / "Kick.wav", src / s.file.path);
        }
        write(src / "Received.sonora", s.bytes());
        std::string why, caution;
        const fs::path out = tmp.path / "Copy" / "Copy.sonora";
        CHECK(!f.save_as(out.string(), s.bytes(), s.h.current(), src.string(), true, why, caution));
        CHECK(mentions(why, "link"));
        CHECK(!fs::exists(tmp.path / "Copy" / s.file.path));
        CHECK(!fs::exists(tmp.path / "Copy"));
    }
    return 0;
}

// A named file found in neither folder, or with another size, refuses the
// save by name; one the replaced file already named (lost before, said at
// open) does not block saving. A copy whose bytes are not the digest's is
// refused: the name is the hash.
int test_missing_or_wrong_file() {
    TempDir tmp;
    const Sampler s;
    SessionFolder f;
    const fs::path engine_dir = tmp.path / "Untitled";
    std::string why, caution;
    f.write_states(engine_dir.string(), {{s.sha, kBlob}}, why);
    const fs::path file = tmp.path / "Mix" / "Mix.sonora";
    CHECK(!f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()));
    CHECK(mentions(why, "plugin file") && mentions(why, "Kick.wav"));
    CHECK(!fs::exists(tmp.path / "Mix"));

    write(engine_dir / s.file.path, kSample + "!");
    CHECK(!f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()));
    CHECK(mentions(why, "Kick.wav"));

    std::string torn = kSample;
    torn[0] = 'X';
    write(engine_dir / s.file.path, torn);
    CHECK(!f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()));
    CHECK(mentions(why, "does not match"));

    write(engine_dir / s.file.path, kSample);
    CHECK(f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()));
    fs::remove_all(tmp.path / "Mix" / "Plugin Files");
    fs::remove_all(engine_dir);
    CHECK(f.save(file.string(), s.bytes(), s.h.current(), why, caution, engine_dir.string()));
    return 0;
}

// #298 review N1: a host renamed a second name with the same bytes into a
// "<sha256>/" an earlier save had synced. That entry is new too: the folder
// is synced again before the session file names it.
int test_second_name_same_bytes_is_synced() {
    TempDir tmp;
    Sampler s;
    SessionFolder f(recording_sync);
    const fs::path folder = tmp.path / "Mix";
    s.flushed_into(f, folder);
    std::string why, caution;
    CHECK(f.save((folder / "Mix.sonora").string(), s.bytes(), s.h.current(), why, caution, folder.string()));

    const PluginFileRef snare{plugin_files_path(s.file.sha256, "Snare.wav"), s.file.sha256, kSample.size()};
    write(folder / snare.path, kSample);
    const Id insert = s.h.current().edit->tracks[0]->inserts[0].id;
    CHECK(s.h.apply(SetInsertState{insert, StateRef{plugin_state_file(s.sha), s.sha, kBlob.size(), {s.file, snare}}},
                    why));
    g_synced.clear();
    CHECK(f.save((folder / "Mix.sonora").string(), s.bytes(), s.h.current(), why, caution, folder.string()));
    CHECK(g_synced.count((folder / "Plugin Files" / s.file.sha256).string()) == 1);
    // Nothing new since: nothing synced again.
    g_synced.clear();
    CHECK(f.save((folder / "Mix.sonora").string(), s.bytes(), s.h.current(), why, caution, folder.string()));
    CHECK(g_synced.count((folder / "Plugin Files" / s.file.sha256).string()) == 0);
    return 0;
}

// A flush (ADR-0029 slice 2b): the files the hosts say they saved are
// checked to be there with their size, through no link, and their folders
// synced; one missing, of another size or behind a link refuses the flush.
int test_flush_checks_and_syncs() {
    TempDir tmp;
    const Sampler s;
    const fs::path folder = tmp.path / "Mix";
    std::map<std::string, PluginFileRef> refs{{s.file.path, s.file}};
    g_synced.clear();
    SessionFolder f(recording_sync);
    std::string why;
    CHECK(!f.sync_plugin_files(folder.string(), refs, why));
    CHECK(mentions(why, "did not leave") && mentions(why, "Kick.wav"));
    write(folder / s.file.path, kSample + "!");
    CHECK(!f.sync_plugin_files(folder.string(), refs, why));
    write(folder / s.file.path, kSample);
    CHECK(f.sync_plugin_files(folder.string(), refs, why));
    CHECK(g_synced.count((folder / "Plugin Files").string()) == 1);
    CHECK(g_synced.count((folder / "Plugin Files" / s.file.sha256).string()) == 1);

    const fs::path other = tmp.path / "Other";
    write(tmp.path / "secret" / "Kick.wav", kSample);
    fs::create_directories(other / "Plugin Files" / s.file.sha256);
    fs::create_symlink(tmp.path / "secret" / "Kick.wav", other / s.file.path);
    CHECK(!f.sync_plugin_files(other.string(), refs, why));
    CHECK(mentions(why, "link"));
    return 0;
}

// #298 review N3: a copy across volumes went through "<basename>.part",
// which passes NAME_MAX for a basename of 251 to 255 bytes, so such a
// session could never be saved to a disk without hard links. Needs a
// source on another filesystem than the temp folder (/dev/shm in the
// build container); skipped, and said, without one.
int test_long_name_copies() {
    TempDir tmp;
    const fs::path shm = fs::path("/dev/shm") / ("sonora-plugin-files-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(shm, ec);
    struct stat a {}, b {};
    if (ec || ::stat(shm.c_str(), &a) != 0 || ::stat(tmp.path.c_str(), &b) != 0 || a.st_dev == b.st_dev) {
        std::printf("  long_name_copies: SKIP, no second filesystem at /dev/shm\n");
        fs::remove_all(shm, ec);
        return 0;
    }
    Sampler s;
    const std::string name = std::string(251, 'k') + ".wav";
    const PluginFileRef longest{plugin_files_path(s.file.sha256, name), s.file.sha256, kSample.size()};
    const Id insert = s.h.current().edit->tracks[0]->inserts[0].id;
    std::string why, caution;
    CHECK(s.h.apply(SetInsertState{insert, StateRef{plugin_state_file(s.sha), s.sha, kBlob.size(), {longest}}}, why));
    SessionFolder f;
    CHECK(f.write_states(shm.string(), {{s.sha, kBlob}}, why));
    write(shm / longest.path, kSample);
    const fs::path file = tmp.path / "Mix" / "Mix.sonora";
    const bool saved = f.save(file.string(), s.bytes(), s.h.current(), why, caution, shm.string());
    if (!saved) std::fprintf(stderr, "  %s\n", why.c_str());
    CHECK(saved);
    CHECK(read(tmp.path / "Mix" / longest.path) == kSample);
    CHECK(!fs::exists(tmp.path / "Mix" / "Plugin Files" / s.file.sha256 / ".incoming-sonora.part"));
    fs::remove_all(shm, ec);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_in_place_is_synced();
    failed += test_files_follow_the_session_file();
    failed += test_links_are_never_followed();
    failed += test_missing_or_wrong_file();
    failed += test_second_name_same_bytes_is_synced();
    failed += test_flush_checks_and_syncs();
    failed += test_long_name_copies();
    std::printf("%s\n", failed == 0 ? "session_folder_plugin_files: all passed" : "session_folder_plugin_files: FAILED");
    return failed == 0 ? 0 : 1;
}
