// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session's copies (daw.model.session_mgmt; session-format.md,
// "Saving" step 5 and "Autosave"; ADR-0018 §5 and its update of
// 2026-10-27): rotated backups kept from the bytes a save replaced, the
// newest N, pruning nothing but this session's rotated backups and never a
// symlink; the autosave beside them, one generation past the file; Save As
// linking the media into a new folder and leaving the old one as it was,
// refusing what it must and taking back a failed attempt; and a writer
// killed mid-write leaving the old file or the new one, never neither.
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/session_files/session_folder.h"

using namespace model_test;
using sonora::session_files::SaveAsReport;
using sonora::session_files::SessionFolder;
namespace fs = std::filesystem;
namespace sf = sonora::session_files;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-session-copies-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
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

// A session with one audio track and `sources` recorded sources.
struct Audio {
    History h{new_session(settings())};
    std::vector<std::string> paths;
    explicit Audio(int sources = 1) {
        Ids ids(h.current());
        std::string why;
        const AddTrack t = add_track(ids, "Gtr");
        h.apply(t, why);
        for (int i = 0; i < sources; ++i) {
            const AddSource s = add_source(ids, 48000, 1, t.track);
            h.apply(s, why);
            paths.push_back(s.source.path);
        }
    }
    // Another revision, so the next save's bytes differ.
    void rename(const std::string& name) {
        SetSettings s{h.current().settings};
        s.settings.name = name;
        std::string why;
        h.apply(s, why);
    }
    std::string bytes(std::uint64_t generation = 1) const {
        std::string out, error;
        serialize(h.current(), WriteInfo{"test", generation}, out, error);
        return out;
    }
};

long long fake_now = 1'800'000'000;   // 2027-01-15
long long fake_clock() { return fake_now; }

std::set<std::string> names(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        out.insert(it->path().filename().string());
    }
    return out;
}

// Every file under `dir` with its bytes, to tell that nothing changed.
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type;
        const fs::file_status s = it->symlink_status(type);
        const std::string rel = it->path().lexically_relative(dir).generic_string();
        if (fs::is_symlink(s)) out[rel] = "-> " + fs::read_symlink(it->path(), type).string();
        else if (fs::is_regular_file(s)) out[rel] = read(it->path());
        else out[rel] = "/";
    }
    return out;
}

int test_rotated_backup_names() {
    CHECK(sf::rotated_backup_name("Mix.20270115-080000.sonora", "Mix"));
    CHECK(sf::rotated_backup_name("Mix.20270115-080000-2.sonora", "Mix"));
    CHECK(sf::rotated_backup_name("Mix.20270115-080000-123456.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.autosave.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.v9.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix 2.20270115-080000.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Other.20270115-080000.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.20270115-080000-1.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.20270115-080000-02.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.20270115-0800000.sonora", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.20270115-080000.sonora.tmp", "Mix"));
    CHECK(!sf::rotated_backup_name("Mix.2027011a-080000.sonora", "Mix"));
    CHECK(sf::autosave_file_of("/s/Mix/Mix.sonora") == "/s/Mix/Session File Backups/Mix.autosave.sonora");
    return 0;
}

// Step 5: each save keeps the bytes it replaced, newest N, by name.
int test_backups_rotate() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path backups = folder / "Session File Backups";
    Audio a;
    SessionFolder f;
    f.set_backups(3);
    f.set_clock(fake_clock);
    std::string why, caution;
    std::vector<std::string> saved;
    for (int i = 0; i < 6; ++i) {
        a.rename("Mix " + std::to_string(i));
        saved.push_back(a.bytes(static_cast<std::uint64_t>(i + 1)));
        CHECK(f.save(file.string(), saved.back(), a.h.current(), why, caution));
        CHECK(caution.empty());
        fake_now += 60;
    }
    // Six saves replaced five files; the three newest are kept.
    const std::set<std::string> kept = names(backups);
    CHECK(kept.size() == 3);
    std::vector<std::string> in_order(kept.begin(), kept.end());
    CHECK(read(backups / in_order[0]) == saved[2]);
    CHECK(read(backups / in_order[1]) == saved[3]);
    CHECK(read(backups / in_order[2]) == saved[4]);
    CHECK(read(file) == saved[5]);
    for (const std::string& n : kept) CHECK(sf::rotated_backup_name(n, "Mix"));

    // A save of the same bytes replaces nothing and keeps nothing.
    CHECK(f.save(file.string(), saved.back(), a.h.current(), why, caution));
    CHECK(names(backups) == kept);

    // Two saves in one second do not overwrite each other's backup.
    a.rename("Again");
    const std::string again = a.bytes(7);
    CHECK(f.save(file.string(), again, a.h.current(), why, caution));
    a.rename("Again 2");
    CHECK(f.save(file.string(), a.bytes(8), a.h.current(), why, caution));
    const std::set<std::string> now = names(backups);
    CHECK(now.size() == 3);
    // '-' sorts before '.', so the "-2" name is found, not taken as last.
    std::string second;
    for (const std::string& n : now) {
        if (n.find("-2.sonora") != std::string::npos) second = n;
    }
    CHECK(!second.empty());
    CHECK(read(backups / second) == again);

    // None kept: none written.
    SessionFolder none;
    none.set_backups(0);
    const fs::path other = tmp.path / "Bare" / "Bare.sonora";
    CHECK(none.save(other.string(), saved[0], a.h.current(), why, caution));
    CHECK(none.save(other.string(), saved[1], a.h.current(), why, caution));
    CHECK(!fs::exists(other.parent_path() / "Session File Backups"));
    return 0;
}

// A clock set back (no network time) still names the new backup newest,
// so pruning never removes the backup just written.
int test_backup_after_the_clock_went_back() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    Audio a;
    SessionFolder f;
    f.set_backups(2);
    f.set_clock(fake_clock);
    std::string why, caution;
    CHECK(f.save(file.string(), a.bytes(1), a.h.current(), why, caution));
    a.rename("B");
    CHECK(f.save(file.string(), a.bytes(2), a.h.current(), why, caution));
    a.rename("C");
    CHECK(f.save(file.string(), a.bytes(3), a.h.current(), why, caution));
    fake_now -= 365 * 86400;
    a.rename("D");
    CHECK(f.save(file.string(), a.bytes(4), a.h.current(), why, caution));
    const std::set<std::string> kept = names(folder / "Session File Backups");
    CHECK(kept.size() == 2);
    // The newest name holds what the last save replaced ("C"), the other "B".
    const std::vector<std::string> v(kept.begin(), kept.end());
    CHECK(read(folder / "Session File Backups" / v[1]).find("\"C\"") != std::string::npos);
    CHECK(read(folder / "Session File Backups" / v[0]).find("\"B\"") != std::string::npos);
    fake_now += 365 * 86400;
    return 0;
}

// Pruning deletes this session's rotated backups and nothing else: not the
// autosave, a pre-migration copy, another stem's backups, a file it cannot
// parse the name of, nor a symlink named as a backup (#151's lesson), whose
// target is the operator's.
int test_pruning_removes_only_backups() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path backups = folder / "Session File Backups";
    const fs::path precious = tmp.path / "Elsewhere" / "precious.sonora";
    write(precious, "keep me");
    write(backups / "Mix.autosave.sonora", "autosave");
    write(backups / "Mix.v5.sonora", "v5");
    write(backups / "Other.20200101-000000.sonora", "other");
    write(backups / "Mix.notes.txt", "notes");
    write(backups / "Mix.20200101-000000.sonora.tmp", "tmp");
    fs::create_symlink(precious, backups / "Mix.20000101-000000.sonora");
    fs::create_directories(backups / "Mix.19990101-000000.sonora");   // a folder so named
    for (int i = 0; i < 5; ++i) {
        write(backups / ("Mix.2020010" + std::to_string(i + 1) + "-000000.sonora"), std::to_string(i));
    }
    SessionFolder f;
    std::string caution;
    CHECK(f.prune_backups(folder.string(), "Mix", 2, caution) == 3);
    CHECK(caution.empty());
    const std::set<std::string> left = names(backups);
    CHECK(left.count("Mix.20200104-000000.sonora") == 1);
    CHECK(left.count("Mix.20200105-000000.sonora") == 1);
    CHECK(left.count("Mix.20200101-000000.sonora") == 0);
    CHECK(left.count("Mix.20200103-000000.sonora") == 0);
    CHECK(left.count("Mix.autosave.sonora") == 1);
    CHECK(left.count("Mix.v5.sonora") == 1);
    CHECK(left.count("Other.20200101-000000.sonora") == 1);
    CHECK(left.count("Mix.notes.txt") == 1);
    CHECK(left.count("Mix.20200101-000000.sonora.tmp") == 1);
    CHECK(fs::is_symlink(backups / "Mix.20000101-000000.sonora"));
    CHECK(fs::is_directory(backups / "Mix.19990101-000000.sonora"));
    CHECK(read(precious) == "keep me");
    // Keeping 0 still leaves the symlink and its target.
    CHECK(f.prune_backups(folder.string(), "Mix", 0, caution) == 2);
    CHECK(fs::is_symlink(backups / "Mix.20000101-000000.sonora"));
    CHECK(read(precious) == "keep me");
    return 0;
}

// "Session File Backups" itself a symlink: nothing is pruned through it,
// no backup or autosave is written into it, and the save still lands.
int test_symlinked_backups_folder() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path outside = tmp.path / "Outside";
    for (int i = 0; i < 4; ++i) write(outside / ("Mix.2020010" + std::to_string(i + 1) + "-000000.sonora"), "x");
    fs::create_directories(folder);
    fs::create_directory_symlink(outside, folder / "Session File Backups");
    Audio a;
    SessionFolder f;
    f.set_backups(1);
    std::string why, caution;
    CHECK(f.save(file.string(), a.bytes(1), a.h.current(), why, caution));
    a.rename("B");
    CHECK(f.save(file.string(), a.bytes(2), a.h.current(), why, caution));
    CHECK(read(file) == a.bytes(2));
    CHECK(mentions(caution, "no backup was kept"));
    CHECK(names(outside).size() == 4);
    std::string pruned;
    CHECK(f.prune_backups(folder.string(), "Mix", 0, pruned) == 0);
    CHECK(mentions(pruned, "not pruned"));
    CHECK(names(outside).size() == 4);
    CHECK(!f.autosave(file.string(), a.h.current(), "test", folder.string(), why, caution));
    CHECK(mentions(why, "not written outside"));
    CHECK(names(outside).size() == 4);
    return 0;
}

// The autosave: beside the backups, one generation past the file, not
// rewritten when nothing changed, never rotated, a kept file for cleanup.
int test_autosave() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path autosave = folder / "Session File Backups" / "Mix.autosave.sonora";
    Audio a;
    SessionFolder f;
    std::string why, caution;
    const std::string saved = a.bytes(7);
    CHECK(f.save(file.string(), saved, a.h.current(), why, caution));
    a.rename("Edited");
    CHECK(f.autosave(file.string(), a.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(caution.empty());
    const std::string first = read(autosave);
    std::uint64_t generation = 0;
    CHECK(file_save_generation(first, generation) && generation == 8);
    CHECK(first.find("\"Edited\"") != std::string::npos);
    // The session file is the operator's: an autosave never touches it.
    CHECK(read(file) == saved);
    // Unchanged: not written again (its inode stays).
    struct stat before {}, after {};
    CHECK(::stat(autosave.c_str(), &before) == 0);
    CHECK(f.autosave(file.string(), a.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(::stat(autosave.c_str(), &after) == 0 && before.st_ino == after.st_ino);
    // Changed again: replaced, and no rotated backup of the autosave.
    a.rename("Edited 2");
    CHECK(f.autosave(file.string(), a.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(read(autosave).find("\"Edited 2\"") != std::string::npos);
    for (const std::string& n : names(folder / "Session File Backups")) {
        CHECK(n == "Mix.autosave.sonora" || sf::rotated_backup_name(n, "Mix"));
        CHECK(n.find("autosave.2") == std::string::npos);
    }
    // No session file yet: generation 1.
    const fs::path fresh = tmp.path / "New" / "New.sonora";
    fs::create_directories(fresh.parent_path());
    CHECK(f.autosave(fresh.string(), a.h.current(), "sonora-engine", fresh.parent_path().string(), why, caution));
    CHECK(file_save_generation(read(sf::autosave_file_of(fresh.string())), generation) && generation == 1);
    return 0;
}

// #164 review B1: a restarted engine's first autosave must not overwrite
// the autosave a crash left, which holds the edits made after the last
// save. It is moved aside to a name pruning and the open-time scan ignore,
// and the saver replaces only autosaves it wrote itself from then on.
int test_reopen_keeps_the_crash_autosave() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path autosave = sf::autosave_file_of(file.string());
    const fs::path backups = folder / "Session File Backups";
    std::string why, caution;
    {
        Audio a;
        SessionFolder before;
        CHECK(before.save(file.string(), a.bytes(7), a.h.current(), why, caution));
        a.rename("Crash edits");
        CHECK(before.autosave(file.string(), a.h.current(), "sonora-engine", folder.string(), why, caution));
    }
    const std::string crash = read(autosave);
    CHECK(crash.find("\"Crash edits\"") != std::string::npos);

    // The engine restarted: a new saver, the document as the file holds it
    // plus one edit.
    Audio b;
    b.rename("After reopen");
    SessionFolder after;
    after.set_clock(fake_clock);
    CHECK(after.autosave(file.string(), b.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(read(autosave).find("\"After reopen\"") != std::string::npos);
    std::string kept;
    for (const std::string& n : names(backups)) {
        if (n != autosave.filename().string() && read(backups / n) == crash) kept = n;
    }
    CHECK(!kept.empty());
    CHECK(kept.find("recovered") != std::string::npos);
    CHECK(!sf::rotated_backup_name(kept, "Mix"));
    CHECK(mentions(caution, kept.c_str()));
    // Pruning keeps it, whatever it keeps.
    std::string pruned;
    after.prune_backups(folder.string(), "Mix", 0, pruned);
    CHECK(read(backups / kept) == crash);

    // Its own autosave it replaces, and nothing more is moved aside.
    const std::size_t count = names(backups).size();
    b.rename("Again");
    CHECK(after.autosave(file.string(), b.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(names(backups).size() == count);
    CHECK(read(autosave).find("\"Again\"") != std::string::npos);
    return 0;
}

// #172 review N1: a saver owns an autosave only while its bytes are the ones
// it wrote. Engine A autosaves P, Save As moves it to Q, a second engine
// opens P and autosaves its own edits; when A returns to P those edits are
// moved aside, not overwritten.
int test_returning_to_a_path_does_not_trust_a_foreign_autosave() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path other = tmp.path / "Moved" / "Moved.sonora";
    const fs::path autosave = sf::autosave_file_of(file.string());
    const fs::path backups = folder / "Session File Backups";
    std::string why, caution;
    Audio a;
    SessionFolder first;
    first.set_clock(fake_clock);
    CHECK(first.save(file.string(), a.bytes(7), a.h.current(), why, caution));
    a.rename("A edits");
    CHECK(first.autosave(file.string(), a.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(first.save(other.string(), a.bytes(7), a.h.current(), why, caution));

    Audio b;
    b.rename("Second engine");
    SessionFolder second;
    second.set_clock(fake_clock);
    CHECK(second.autosave(file.string(), b.h.current(), "sonora-engine", folder.string(), why, caution));
    const std::string foreign = read(autosave);
    CHECK(foreign.find("\"Second engine\"") != std::string::npos);

    a.rename("A again");
    CHECK(first.autosave(file.string(), a.h.current(), "sonora-engine", folder.string(), why, caution));
    CHECK(read(autosave).find("\"A again\"") != std::string::npos);
    std::string kept;
    for (const std::string& n : names(backups)) {
        if (n != autosave.filename().string() && read(backups / n) == foreign) kept = n;
    }
    CHECK(!kept.empty());
    return 0;
}

// #164 review B1: two session files in one folder (Pro Tools' habit of
// "Song" and "Song alt") each have their own autosave.
int test_each_session_file_has_its_own_autosave() {
    CHECK(sf::autosave_file_of("/s/Mix/Song.sonora") != sf::autosave_file_of("/s/Mix/Song alt.sonora"));
    CHECK(sf::autosave_file_of("/s/Mix/Song alt.sonora") ==
          "/s/Mix/Session File Backups/Song alt.autosave.sonora");
    // An autosave opened from the backups folder autosaves onto itself.
    CHECK(sf::autosave_file_of("/s/Mix/Session File Backups/Song.autosave.sonora") ==
          "/s/Mix/Session File Backups/Song.autosave.sonora");
    // A rotated backup opened has its session's autosave; the interface's
    // regex gives the same (ui/qt test_edit, test_autosave_names_agree).
    CHECK(sf::autosave_file_of("/s/Mix/Session File Backups/Song alt.20270115-080000-2.sonora") ==
          "/s/Mix/Session File Backups/Song alt.autosave.sonora");
    CHECK(sf::autosave_file_of("/s/Mix/Session File Backups/v1.2.20270115-080000.sonora") ==
          "/s/Mix/Session File Backups/v1.2.autosave.sonora");
    CHECK(sf::autosave_file_of("/s/Mix/Session File Backups/Song.v3.sonora") ==
          "/s/Mix/Session File Backups/Song.v3.autosave.sonora");
    return 0;
}

// #164 review B2: a rotated backup is pruning's to delete, so nothing is
// saved into one; Save As keeps a session opened from it. A pre-migration
// copy or the autosave, which pruning never touches, still saves in place.
int test_no_save_into_a_rotated_backup() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path backups = folder / "Session File Backups";
    Audio a;
    SessionFolder f;
    std::string why, caution;
    CHECK(f.save(file.string(), a.bytes(1), a.h.current(), why, caution));
    const fs::path rotated = backups / "Mix.20200101-000000.sonora";
    write(rotated, a.bytes(1));
    a.rename("An hour of work");
    CHECK(!f.save(rotated.string(), a.bytes(2), a.h.current(), why, caution));
    CHECK(mentions(why, "Save As"));
    CHECK(read(rotated).find("\"An hour of work\"") == std::string::npos);
    const fs::path second = backups / "Mix.20200101-000000-2.sonora";
    write(second, a.bytes(1));
    CHECK(!f.save(second.string(), a.bytes(2), a.h.current(), why, caution));
    const fs::path other = backups / "Mix alt.20200101-000000.sonora";
    write(other, a.bytes(1));
    CHECK(!f.save(other.string(), a.bytes(2), a.h.current(), why, caution));
    // Not rotated names: saved in place as before.
    const fs::path autosave = sf::autosave_file_of(file.string());
    write(autosave, a.bytes(1));
    CHECK(f.save(autosave.string(), a.bytes(3), a.h.current(), why, caution));
    return 0;
}

// A source folder with media of every kind Save As meets.
struct Source {
    fs::path folder;
    Audio a{2};
    explicit Source(const fs::path& root) : folder(root / "Old") {
        write(folder / a.paths[0], "take one");
        // a.paths[1] is missing: offline before, offline after.
        write(folder / "Audio Files" / "orphan_07.wav", "a take never placed");
        write(folder / "Audio Files" / "orphan_07.wav.wraps", "wraps");
        write(folder / "Audio Files" / "half.wav.part", "an interrupted copy");
        write(folder / "MIDI Files" / "Keys_01.mid", "MThd");
        write(folder / "Old.sonora", a.bytes(3));
        write(folder / "Session File Backups" / "Old.20200101-000000.sonora", a.bytes(2));
        write(root / "Linked" / "outside.wav", "outside audio");
        fs::create_symlink(root / "Linked" / "outside.wav", folder / "Audio Files" / "linked.wav");
    }
};

// Save As: a new folder whose media are the old one's by hard link, the
// session file last, the old folder untouched.
int test_save_as_links_media_and_leaves_the_source() {
    TempDir tmp;
    Source src(tmp.path);
    const auto before = tree(src.folder);
    const fs::path file = tmp.path / "New" / "New.sonora";
    SessionFolder f;
    std::string why, caution;
    SaveAsReport report;
    CHECK(f.save_as(file.string(), src.a.bytes(4), src.a.h.current(), src.folder.string(), true, why,
                    caution, &report));
    CHECK(read(file) == src.a.bytes(4));
    const fs::path to = tmp.path / "New";
    CHECK(read(to / src.a.paths[0]) == "take one");
    CHECK(read(to / "Audio Files" / "orphan_07.wav") == "a take never placed");
    CHECK(read(to / "Audio Files" / "orphan_07.wav.wraps") == "wraps");
    CHECK(read(to / "MIDI Files" / "Keys_01.mid") == "MThd");
    // The symlink's audio is copied in, not linked: nothing points out.
    CHECK(!fs::is_symlink(to / "Audio Files" / "linked.wav"));
    CHECK(read(to / "Audio Files" / "linked.wav") == "outside audio");
    CHECK(!fs::exists(to / "Audio Files" / "half.wav.part"));
    CHECK(!fs::exists(to / "Session File Backups"));
    struct stat a {}, b {};
    CHECK(::stat((src.folder / src.a.paths[0]).c_str(), &a) == 0);
    CHECK(::stat((to / src.a.paths[0]).c_str(), &b) == 0);
    CHECK(a.st_ino == b.st_ino && a.st_dev == b.st_dev);
    CHECK(report.linked == 4 && report.copied == 1);
    CHECK(report.missing.size() == 1 && report.missing[0] == src.a.paths[1]);
    CHECK(mentions(caution, "already missing"));
    CHECK(tree(src.folder) == before);

    // Save Copy In without the audio: the session file and no media.
    const fs::path bare = tmp.path / "Bare" / "Bare.sonora";
    CHECK(f.save_as(bare.string(), src.a.bytes(4), src.a.h.current(), src.folder.string(), false, why,
                    caution, &report));
    CHECK(fs::exists(bare) && !fs::exists(bare.parent_path() / "Audio Files"));
    CHECK(report.linked == 0 && report.copied == 0);
    CHECK(tree(src.folder) == before);
    return 0;
}

int test_save_as_refusals() {
    TempDir tmp;
    Source src(tmp.path);
    const auto before = tree(src.folder);
    SessionFolder f;
    std::string why, caution;
    const std::string bytes = src.a.bytes(4);
    const Snapshot& doc = src.a.h.current();
    write(tmp.path / "Taken" / "something.txt", "x");
    CHECK(!f.save_as((tmp.path / "Taken" / "Taken.sonora").string(), bytes, doc, src.folder.string(), true,
                     why, caution));
    CHECK(mentions(why, "not empty"));
    CHECK(read(tmp.path / "Taken" / "something.txt") == "x");
    CHECK(!f.save_as((src.folder / "Old 2.sonora").string(), bytes, doc, src.folder.string(), true, why,
                     caution));
    CHECK(mentions(why, "own folder"));
    CHECK(!f.save_as((src.folder / "Inner" / "Inner.sonora").string(), bytes, doc, src.folder.string(),
                     true, why, caution));
    CHECK(!f.save_as((tmp.path / "Old.sonora").string(), bytes, doc, src.folder.string(), true, why,
                     caution));
    CHECK(!f.save_as((src.folder / "Session File Backups" / "X.sonora").string(), bytes, doc,
                     src.folder.string(), true, why, caution));
    CHECK(!f.save_as("relative/X.sonora", bytes, doc, src.folder.string(), true, why, caution));
    fs::create_directory_symlink(tmp.path / "Taken", tmp.path / "Link");
    CHECK(!f.save_as((tmp.path / "Link" / "Link.sonora").string(), bytes, doc, src.folder.string(), true,
                     why, caution));
    CHECK(tree(src.folder) == before);
    // An empty folder made beforehand is fine.
    fs::create_directories(tmp.path / "Empty");
    CHECK(f.save_as((tmp.path / "Empty" / "Empty.sonora").string(), bytes, doc, src.folder.string(), true,
                    why, caution));
    return 0;
}

bool sync_fails_in_new(const std::string& path) {
    return path.find("/New") == std::string::npos || path.find("Audio Files") == std::string::npos;
}

// A Save As that fails partway takes back the folder it made; the source
// is as it was, and the same name can be used again.
int test_failed_save_as_is_taken_back() {
    TempDir tmp;
    Source src(tmp.path);
    const auto before = tree(src.folder);
    const fs::path file = tmp.path / "New" / "New.sonora";
    SessionFolder failing(sync_fails_in_new);
    std::string why, caution;
    CHECK(!failing.save_as(file.string(), src.a.bytes(4), src.a.h.current(), src.folder.string(), true,
                           why, caution));
    CHECK(mentions(why, "cannot sync"));
    CHECK(!fs::exists(tmp.path / "New"));
    CHECK(tree(src.folder) == before);
    SessionFolder f;
    CHECK(f.save_as(file.string(), src.a.bytes(4), src.a.h.current(), src.folder.string(), true, why,
                    caution));
    CHECK(fs::exists(file));
    // A session that does not parse is refused once the media are in; the
    // folder made for it is taken back too.
    CHECK(!f.save_as((tmp.path / "Bad" / "Bad.sonora").string(), "{not json", src.a.h.current(),
                     src.folder.string(), true, why, caution));
    CHECK(!fs::exists(tmp.path / "Bad"));
    return 0;
}

// #164 review: only what an interrupted copy leaves, "<media name>.part"
// with no file of that name beside it, in subfolders too as save_as copies
// them; never a symlink, file or directory.
int test_partial_copies_swept() {
    TempDir tmp;
    const fs::path folder = tmp.path / "S";
    write(folder / "Audio Files" / "a.wav.part", "x");
    write(folder / "Audio Files" / "Imported" / "deep" / "b.wav.part", "x");
    write(folder / "MIDI Files" / "k.mid.part", "x");
    write(folder / "Audio Files" / "c.wav.part", "not a copy's");
    write(folder / "Audio Files" / "c.wav", "keep");
    write(folder / "Audio Files" / "notes.part", "the operator's");
    write(folder / "Audio Files" / "x.wav.part.part", "keep");
    write(folder / "Audio Files" / "x.wav.tmp.part", "keep");
    write(tmp.path / "elsewhere.wav.part", "keep");
    fs::create_symlink(tmp.path / "elsewhere.wav.part", folder / "Audio Files" / "link.wav.part");
    write(tmp.path / "Outside" / "o.wav.part", "keep");
    fs::create_directory_symlink(tmp.path / "Outside", folder / "Audio Files" / "Outside");
    CHECK(sf::sweep_partial_copies(folder.string()) == 3);
    CHECK(!fs::exists(folder / "Audio Files" / "a.wav.part"));
    CHECK(!fs::exists(folder / "Audio Files" / "Imported" / "deep" / "b.wav.part"));
    CHECK(!fs::exists(folder / "MIDI Files" / "k.mid.part"));
    CHECK(read(folder / "Audio Files" / "c.wav.part") == "not a copy's");
    CHECK(read(folder / "Audio Files" / "c.wav") == "keep");
    CHECK(read(folder / "Audio Files" / "notes.part") == "the operator's");
    CHECK(fs::exists(folder / "Audio Files" / "x.wav.part.part"));
    CHECK(fs::exists(folder / "Audio Files" / "x.wav.tmp.part"));
    CHECK(fs::is_symlink(folder / "Audio Files" / "link.wav.part"));
    CHECK(read(tmp.path / "elsewhere.wav.part") == "keep");
    CHECK(read(tmp.path / "Outside" / "o.wav.part") == "keep");
    return 0;
}

bool sync_fails_in_empty(const std::string& path) {
    return path.find("/Empty") == std::string::npos || path.find("Audio Files") == std::string::npos;
}

// #164 review: a Save As that fails in an empty folder made beforehand
// takes back what it put there and nothing else, so the retry is not
// refused "not empty".
int test_failed_save_as_in_an_empty_folder() {
    TempDir tmp;
    Source src(tmp.path);
    const fs::path empty = tmp.path / "Empty";
    fs::create_directories(empty);
    const fs::path file = empty / "Empty.sonora";
    SessionFolder failing(sync_fails_in_empty);
    std::string why, caution;
    CHECK(!failing.save_as(file.string(), src.a.bytes(4), src.a.h.current(), src.folder.string(), true, why,
                           caution));
    CHECK(mentions(why, "cannot sync"));
    CHECK(fs::is_directory(empty) && fs::is_empty(empty));
    SessionFolder f;
    CHECK(f.save_as(file.string(), src.a.bytes(4), src.a.h.current(), src.folder.string(), true, why,
                    caution));
    CHECK(read(file) == src.a.bytes(4));
    return 0;
}

// #164 review: the saver's own autosave is still its own after a Save As
// took the saver to another folder and back; only a crash's is moved aside.
int test_own_autosave_survives_a_folder_switch() {
    TempDir tmp;
    const fs::path file = tmp.path / "Mix" / "Mix.sonora";
    const fs::path other = tmp.path / "Other" / "Other.sonora";
    Audio a;
    SessionFolder f;
    std::string why, caution;
    CHECK(f.save(file.string(), a.bytes(1), a.h.current(), why, caution));
    a.rename("Edited");
    CHECK(f.autosave(file.string(), a.h.current(), "sonora-engine", file.parent_path().string(), why, caution));
    CHECK(f.save(other.string(), a.bytes(1), a.h.current(), why, caution));
    a.rename("Back again");
    CHECK(f.autosave(file.string(), a.h.current(), "sonora-engine", file.parent_path().string(), why, caution));
    CHECK(f.set_aside().empty());
    for (const std::string& n : names(file.parent_path() / "Session File Backups")) {
        CHECK(n.find("recovered") == std::string::npos);
    }
    CHECK(read(sf::autosave_file_of(file.string())).find("\"Back again\"") != std::string::npos);
    return 0;
}

// A writer killed at an arbitrary moment (SIGKILL: no handler, no flush)
// leaves the session file as it was or as it was going to be, parseable,
// and never missing; the same for the autosave.
int test_kill_mid_write_leaves_a_whole_file() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    const fs::path autosave = folder / "Session File Backups" / "Mix.autosave.sonora";
    // Large enough that a write takes a while: many tracks.
    History h{new_session(settings())};
    {
        Ids ids(h.current());
        std::string why;
        for (int i = 0; i < 400; ++i) h.apply(add_track(ids, "Track " + std::to_string(i)), why);
    }
    std::string a, b, error;
    CHECK(serialize(h.current(), WriteInfo{"a", 1}, a, error));
    SetSettings s{h.current().settings};
    s.settings.name = "Renamed";
    std::string why;
    CHECK(h.apply(s, why));
    CHECK(serialize(h.current(), WriteInfo{"b", 2}, b, error));
    {
        SessionFolder f;
        std::string caution;
        CHECK(f.save(file.string(), a, h.current(), why, caution));
        CHECK(f.autosave(file.string(), h.current(), "a", folder.string(), why, caution));
    }
    int kills_during = 0;
    for (int round = 0; round < 24; ++round) {
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            SessionFolder f;
            f.set_backups(2);
            std::string w, c;
            for (int i = 0;; ++i) {
                f.save(file.string(), (i % 2) ? a : b, h.current(), w, c);
                f.autosave(file.string(), h.current(), (i % 2) ? "a" : "b", folder.string(), w, c);
            }
        }
        ::usleep(static_cast<useconds_t>(2000 + round * 1500));
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        if (WIFSIGNALED(status)) ++kills_during;
        const std::string now = read(file);
        CHECK(now == a || now == b);
        Loaded loaded;
        LoadError err;
        CHECK(deserialize(now, loaded, err));
        const std::string as = read(autosave);
        std::uint64_t g = 0;
        CHECK(file_save_generation(as, g));
        CHECK(fs::exists(file));
    }
    CHECK(kills_during == 24);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_rotated_backup_names);
    t(test_backups_rotate);
    t(test_backup_after_the_clock_went_back);
    t(test_pruning_removes_only_backups);
    t(test_symlinked_backups_folder);
    t(test_autosave);
    t(test_reopen_keeps_the_crash_autosave);
    t(test_returning_to_a_path_does_not_trust_a_foreign_autosave);
    t(test_each_session_file_has_its_own_autosave);
    t(test_no_save_into_a_rotated_backup);
    t(test_save_as_links_media_and_leaves_the_source);
    t(test_save_as_refusals);
    t(test_failed_save_as_is_taken_back);
    t(test_partial_copies_swept);
    t(test_failed_save_as_in_an_empty_folder);
    t(test_own_autosave_survives_a_folder_switch);
    t(test_kill_mid_write_leaves_a_whole_file);
    if (failed != 0) {
        std::fprintf(stderr, "session_copies: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("session_copies: %d passed\n", run);
    return 0;
}
