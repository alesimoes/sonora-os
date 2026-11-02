// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session templates on disk (ADR-0030), through the same SessionFolder the
// engine's saver runs: Save as Template is a Save Copy In without the audio
// of the template's bytes, so the template folder holds its session file and
// the plugin states it names and no take; New from Template is a Save As out
// of the template folder, so the new session folder is self-contained and
// the template is left as it was; a template whose state file is gone makes
// no session folder.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/sha256.h"

using namespace model_test;
using sonora::session_files::SessionFolder;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-session-templates-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
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

std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string rel = it->path().lexically_relative(dir).generic_string();
        out[rel] = fs::is_regular_file(it->symlink_status()) ? read(it->path()) : "/";
    }
    return out;
}

bool holds_files(const fs::path& dir) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->symlink_status())) return true;
    return false;
}

// A tracking session: a track with a take placed on it and an insert whose
// state a flush has written into the session's folder.
struct Tracked {
    History h{new_session(settings())};
    std::string blob = "a compressor's state";
    std::string sha = sonora::sha256_hex(blob);
    std::string take;
    Id insert = 0;

    bool build(const fs::path& folder) {
        Ids ids(h.current());
        std::string why;
        const AddTrack t = add_track(ids, "Vox");
        if (!h.apply(t, why)) return false;
        const AddSource s = add_source(ids, 48000, 1, t.track);
        if (!h.apply(s, why)) return false;
        take = s.source.path;
        if (!h.apply(PlaceRegion{t.playlist, region(ids(), s.source.id, 0, 48000), 0}, why)) return false;
        AddInsert in;
        in.track = t.track;
        in.insert = insert = ids();
        in.plugin = PluginRef{PluginFormat::Lv2, "urn:test#comp", "Comp", "1"};
        in.state = StateRef{plugin_state_file(sha), sha, blob.size()};
        if (!h.apply(in, why)) return false;
        write(folder / take, "RIFF....WAVE");
        SessionFolder f;
        return f.write_states(folder.string(), {{sha, blob}}, why);
    }
};

std::string bytes_of(const Snapshot& s) {
    std::string out, error;
    if (!serialize(s, WriteInfo{"sonora-ui", 1}, out, error)) std::fprintf(stderr, "  %s\n", error.c_str());
    return out;
}

int test_save_as_template_then_new_from_it() {
    TempDir tmp;
    const fs::path mix = tmp.path / "Sessions" / "Mix";
    Tracked session;
    CHECK(session.build(mix));
    std::string why, caution;
    {
        SessionFolder f;
        CHECK(f.save((mix / "Mix.sonora").string(), bytes_of(session.h.current()), session.h.current(), why,
                     caution, mix.string()));
    }
    const auto mix_before = tree(mix);

    // Save as Template: the engine's Save Copy In, no audio, of the
    // template's bytes; the document beside them is the engine's own.
    const fs::path templates = tmp.path / "Session Templates";
    const fs::path tfile = templates / "Vocal Setup" / "Vocal Setup.sonora";
    Snapshot t = template_of(session.h.current());
    t.settings.name = "Vocal Setup";
    {
        SessionFolder f;
        CHECK(f.save_as(tfile.string(), bytes_of(t), session.h.current(), mix.string(), false, why, caution));
    }
    CHECK(tree(mix) == mix_before);
    CHECK(read(tfile.parent_path() / plugin_state_file(session.sha)) == session.blob);
    CHECK(!fs::exists(tfile.parent_path() / session.take));
    CHECK(!fs::exists(tfile.parent_path() / "Audio Files") || !holds_files(tfile.parent_path() / "Audio Files"));
    Loaded tl;
    LoadError le;
    CHECK(deserialize(read(tfile), tl, le));
    CHECK(tl.file_version == kFormatVersion && tl.snapshot.sources->sources.empty());
    CHECK(tl.snapshot.edit->tracks.size() == 1 && tl.snapshot.edit->tracks[0]->playlists[0]->regions.empty());
    CHECK(tl.snapshot.settings.session_id.empty());
    const auto template_before = tree(tfile.parent_path());

    // New from Template: the engine opens the template's folder with the
    // new session's document, then Saves As into the sessions folder.
    const std::string id = new_session_id();
    const Snapshot song = session_from_template(tl.snapshot, "Song", id);
    const fs::path sfile = tmp.path / "Sessions" / "Song" / "Song.sonora";
    {
        SessionFolder f;
        CHECK(f.save_as(sfile.string(), bytes_of(song), song, tfile.parent_path().string(), true, why, caution));
    }
    CHECK(tree(tfile.parent_path()) == template_before);
    const fs::path song_dir = sfile.parent_path();
    CHECK(read(song_dir / plugin_state_file(session.sha)) == session.blob);
    CHECK(!fs::exists(song_dir / session.take));
    Loaded sl;
    CHECK(deserialize(read(sfile), sl, le));
    CHECK(sl.snapshot.settings.name == "Song" && sl.snapshot.settings.session_id == id);
    const InsertMix* m = sl.snapshot.mix.find_insert(session.insert);
    CHECK(m != nullptr && m->state && m->state->sha256 == session.sha);
    CHECK(sonora::sha256_hex(read(song_dir / m->state->file)) == m->state->sha256);
    return 0;
}

// The state a template names is gone (deleted by hand): New from Template
// is refused by name, and no half-made session folder is left behind.
int test_new_from_a_template_missing_its_state() {
    TempDir tmp;
    const fs::path mix = tmp.path / "Mix";
    Tracked session;
    CHECK(session.build(mix));
    const fs::path tdir = tmp.path / "Session Templates" / "Broken";
    std::string why, caution;
    {
        SessionFolder f;
        CHECK(f.save_as((tdir / "Broken.sonora").string(), bytes_of(template_of(session.h.current())),
                        session.h.current(), mix.string(), false, why, caution));
    }
    fs::remove(tdir / plugin_state_file(session.sha));
    Loaded tl;
    LoadError le;
    CHECK(deserialize(read(tdir / "Broken.sonora"), tl, le));
    const Snapshot song = session_from_template(tl.snapshot, "Song", new_session_id());
    const fs::path sfile = tmp.path / "Sessions" / "Song" / "Song.sonora";
    SessionFolder f;
    CHECK(!f.save_as(sfile.string(), bytes_of(song), song, tdir.string(), true, why, caution));
    CHECK(mentions(why, session.sha.substr(0, 12).c_str()));
    CHECK(!fs::exists(sfile.parent_path()));
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_save_as_template_then_new_from_it();
    failures += test_new_from_a_template_missing_its_state();
    if (failures == 0) std::printf("session template folder tests passed\n");
    return failures == 0 ? 0 : 1;
}
