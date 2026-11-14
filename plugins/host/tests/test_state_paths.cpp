// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a save may copy out of a plugin's own files (#276 review N1). The
// files are made by plugin code, so a GetState without a cap could copy and
// hash until the disk is full, and be killed half way by the engine's State
// budget. Each limit is shrunk here (CopyLimits) so the test writes a few
// kilobytes: a file over the per-file cap, files that take one save past the
// total, and a destination with too little room. Each fails that instance's
// save with a text naming the cap, leaves no ".incoming-*" behind, and does
// not touch another instance's save into the same folder.
#include "state_paths.h"

#include "sonora/sha256.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <iterator>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace pc = sonora::plugin::control;
using sonora::plugin_host::CopyLimits;
using sonora::plugin_host::StatePaths;

static int g_failed = 0;

static void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

static void write_file(const fs::path& p, std::size_t bytes, char fill) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(bytes, fill);
}

static bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Nothing half copied: no ".incoming-*" anywhere under the save folder.
static bool no_incoming(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.path().filename().string().rfind(".incoming-", 0) == 0) return false;
    return true;
}

// Names `files` (absolute) through mapPath as one save() does, and returns
// end_save's verdict.
static bool save(StatePaths& sp, const fs::path& into, const std::vector<fs::path>& files,
                 std::vector<pc::StateFile>& listed, std::string& error) {
    sp.begin_save(into.string(), false);
    for (const fs::path& f : files) sp.free_path(sp.abstract_path(f.c_str()));
    return sp.end_save(listed, error);
}

int main() {
    const fs::path base = fs::temp_directory_path() / ("sonora-state-caps-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path scratch = base / "scratch", session = base / "Session";
    fs::create_directories(session);

    CopyLimits small;
    small.file_bytes = 1000;
    small.total_bytes = 2500;
    small.margin_bytes = 512;
    uint64_t room = uint64_t{1} << 40;
    small.free_space = [&room](const std::string&, uint64_t& free) {
        free = room;
        return true;
    };

    std::printf("state_paths: per-file cap\n");
    {
        StatePaths sp((scratch / "a").string(), session.string(), small);
        write_file(scratch / "a" / "big.bin", 1001, 'b');
        write_file(scratch / "a" / "edge.bin", 1000, 'e');
        std::vector<pc::StateFile> listed;
        std::string error;
        check(!save(sp, session, {scratch / "a" / "big.bin"}, listed, error) && listed.empty(),
              "a file one byte over the per-file cap fails the save");
        std::printf("    text: %s\n", error.c_str());
        check(contains(error, "big.bin") && contains(error, "over the") && contains(error, "one plug-in file may be"),
              "  its text names the file and the cap");
        check(no_incoming(session) && !fs::exists(session / "Plugin Files" / "big.bin"),
              "  nothing partial is left in the session folder");
        check(save(sp, session, {scratch / "a" / "edge.bin"}, listed, error) && listed.size() == 1 &&
                  listed[0].bytes == 1000 && fs::file_size(session / listed[0].path) == 1000,
              "a file at the cap is saved, by the same instance's next save");
    }

    std::printf("state_paths: total cap of one save\n");
    {
        StatePaths sp((scratch / "b").string(), session.string(), small);
        std::vector<fs::path> files;
        for (char c : {'1', '2', '3'}) {
            files.push_back(scratch / "b" / (std::string("part") + c + ".bin"));
            write_file(files.back(), 900, c);
        }
        std::vector<pc::StateFile> listed;
        std::string error;
        check(!save(sp, session, files, listed, error) && listed.empty(),
              "three files of 900 bytes under a 2500-byte total fail the save");
        std::printf("    text: %s\n", error.c_str());
        check(contains(error, "part3.bin") && contains(error, "one save may copy"),
              "  its text names the file past the total, and the cap");
        check(no_incoming(session), "  nothing partial is left");
        check(save(sp, session, {files[0], files[1]}, listed, error) && listed.size() == 2,
              "the total is per save: the next save of two of them passes");
    }

    std::printf("state_paths: free space on the destination\n");
    {
        StatePaths sp((scratch / "c").string(), session.string(), small);
        StatePaths other((scratch / "d").string(), session.string(), small);
        write_file(scratch / "c" / "take.bin", 900, 'c');
        write_file(scratch / "d" / "tiny.bin", 10, 'd');
        room = 900 + 511;  // the copy would leave one byte under the margin
        std::vector<pc::StateFile> listed;
        std::string error;
        check(!save(sp, session, {scratch / "c" / "take.bin"}, listed, error) && listed.empty(),
              "a copy that would leave less than the margin free fails the save");
        std::printf("    text: %s\n", error.c_str());
        check(contains(error, "not enough room") && contains(error, "take.bin") && contains(error, "must stay free"),
              "  its text says there is no room, for which file, and why");
        check(no_incoming(session), "  nothing partial is left");
        room = 10 + 512;
        std::vector<pc::StateFile> theirs;
        std::string their_error;
        check(save(other, session, {scratch / "d" / "tiny.bin"}, theirs, their_error) && theirs.size() == 1 &&
                  fs::file_size(session / theirs[0].path) == 10,
              "another instance's save into the same folder, which fits, is saved");
    }

    std::printf("state_paths: a failed save copies nothing after its failure\n");
    {
        StatePaths sp((scratch / "e").string(), session.string(), small);
        write_file(scratch / "e" / "over.bin", 2000, 'o');
        write_file(scratch / "e" / "after.bin", 20, 'a');
        room = uint64_t{1} << 40;
        std::vector<pc::StateFile> listed;
        std::string error;
        sp.begin_save(session.string(), false);
        sp.free_path(sp.abstract_path((scratch / "e" / "over.bin").c_str()));
        char* after = sp.abstract_path((scratch / "e" / "after.bin").c_str());
        check(after && std::string(after) == (scratch / "e" / "after.bin").string(),
              "a file named after the failure is not copied (named as given)");
        sp.free_path(after);
        check(!sp.end_save(listed, error) && contains(error, "over.bin"), "  and the save reports the first failure");
    }

    // #283 review: the free-space query failing, and free space below the
    // file's own size (the guard before the margin's subtraction).
    std::printf("state_paths: free space unknown, or under the file's size\n");
    {
        CopyLimits blind = small;
        blind.free_space = [](const std::string&, uint64_t&) {
            errno = EIO;
            return false;
        };
        StatePaths sp((scratch / "f").string(), session.string(), blind);
        write_file(scratch / "f" / "take.bin", 900, 'f');
        std::vector<pc::StateFile> listed;
        std::string error;
        check(!save(sp, session, {scratch / "f" / "take.bin"}, listed, error) && listed.empty(),
              "a free-space query that fails refuses the copy");
        std::printf("    text: %s\n", error.c_str());
        check(contains(error, "cannot tell how much room"), "  its text says the room is unknown");
        StatePaths under((scratch / "f").string(), session.string(), small);
        room = 500;  // under the 900 bytes themselves: free - size would wrap
        check(!save(under, session, {scratch / "f" / "take.bin"}, listed, error) && contains(error, "not enough room"),
              "free space under the file's size is no room, not a wrapped subtraction");
        check(no_incoming(session), "  nothing partial is left");
        room = uint64_t{1} << 40;
    }

    std::printf("state_paths: a source that grows while it is copied\n");
    {
        CopyLimits growing = small;
        const fs::path src = scratch / "g" / "loop.bin";
        bool grown = false;
        // Read before every chunk: the plugin appends after the size was taken.
        growing.clock = [&] {
            if (!grown) std::ofstream(src, std::ios::binary | std::ios::app) << std::string(200, 'x');
            grown = true;
            return uint64_t{0};
        };
        StatePaths sp((scratch / "g").string(), session.string(), growing);
        write_file(src, 900, 'g');
        grown = true;  // not while begin_save starts the clock
        std::vector<pc::StateFile> listed;
        std::string error;
        sp.begin_save(session.string(), false);
        grown = false;
        sp.free_path(sp.abstract_path(src.c_str()));
        check(!sp.end_save(listed, error) && listed.empty() && contains(error, "grew while it was being saved"),
              "a file that grows past its checked size fails the save");
        std::printf("    text: %s\n", error.c_str());
        check(no_incoming(session), "  and leaves nothing behind");
    }

    std::printf("state_paths: a copy that runs out of the save's time\n");
    {
        CopyLimits slow;
        slow.file_bytes = slow.total_bytes = uint64_t{16} << 20;
        slow.margin_bytes = 0;
        slow.free_space = small.free_space;
        slow.budget_ms = 2500;
        uint64_t t = 0;
        slow.clock = [&t] { return t += 1000; };  // a second per chunk of 1 MiB
        StatePaths sp((scratch / "h").string(), session.string(), slow);
        write_file(scratch / "h" / "patch.bin", std::size_t{4} << 20, 'h');
        std::vector<pc::StateFile> listed;
        std::string error;
        check(!save(sp, session, {scratch / "h" / "patch.bin"}, listed, error) && listed.empty(),
              "a copy still running at the budget stops and fails the save");
        std::printf("    text: %s\n", error.c_str());
        check(contains(error, "patch.bin") && contains(error, "too slow"), "  its text names the file and why");
        check(no_incoming(session), "  and leaves no .incoming-* to be killed over");
    }

    std::printf("state_paths: makePath through a symlink out of its folder (#276 review N6)\n");
    {
        const fs::path elsewhere = base / "elsewhere";
        fs::create_directories(elsewhere);
        fs::create_directories(scratch / "i");
        fs::create_directory_symlink(elsewhere, scratch / "i" / "out");
        fs::create_symlink(elsewhere / "f.wav", scratch / "i" / "link.wav");
        StatePaths sp((scratch / "i").string(), session.string(), small);
        char* through = sp.make_path("out/sub/x.wav");
        check(!through && sp.refused() == 1, "a parent that is a symlink out of the folder: refused and counted");
        sp.free_path(through);
        char* last = sp.make_path("link.wav");
        check(!last && sp.refused() == 2, "a last component that is a symlink out: refused and counted");
        sp.free_path(last);
        check(contains(sp.refusal_text(), "a symlink out of its folder"), "  the text says so");
        check(fs::is_empty(elsewhere), "  nothing was made through either");
    }

    std::printf("state_paths: a symlinked Plugin Files/ or <sha256>/ (#276 review N4)\n");
    {
        const fs::path out1 = base / "out1", out2 = base / "out2";
        const fs::path s1 = base / "S1", s2 = base / "S2";
        fs::create_directories(out1);
        fs::create_directories(out2);
        fs::create_directories(s1);
        fs::create_directories(s2 / "Plugin Files");
        fs::create_directory_symlink(out1, s1 / "Plugin Files");
        write_file(scratch / "j" / "kit.bin", 100, 'k');
        sonora::Sha256 h;
        const std::string bytes(100, 'k');
        h.update(bytes.data(), bytes.size());
        fs::create_directory_symlink(out2, s2 / "Plugin Files" / h.hex());
        StatePaths sp((scratch / "j").string(), s1.string(), small);
        std::vector<pc::StateFile> listed;
        std::string error;
        check(!save(sp, s1, {scratch / "j" / "kit.bin"}, listed, error) && contains(error, "symlink"),
              "Plugin Files/ a symlink: the save fails");
        std::printf("    text: %s\n", error.c_str());
        check(fs::is_empty(out1), "  nothing written through it");
        check(!save(sp, s2, {scratch / "j" / "kit.bin"}, listed, error) && contains(error, "symlink"),
              "<sha256>/ a symlink: the save fails");
        std::printf("    text: %s\n", error.c_str());
        check(fs::is_empty(out2) && no_incoming(s2), "  nothing written through it, nothing left");
    }

    std::printf("state_paths: saved files stay what their hash says (#276 review N3)\n");
    {
        const fs::path s = base / "S3";
        fs::create_directories(s);
        write_file(scratch / "k" / "loop.wav", 64, 'l');
        std::vector<pc::StateFile> listed;
        std::string error;
        std::string rel;
        {
            StatePaths sp((scratch / "k").string(), s.string(), small);
            check(save(sp, s, {scratch / "k" / "loop.wav"}, listed, error) && listed.size() == 1, "saved");
            if (!listed.empty()) rel = listed[0].path;
        }
        const fs::path saved = s / rel;
        check(!rel.empty() && (fs::status(saved).permissions() & fs::perms::all) ==
                                  (fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read),
              "a saved plugin file is read-only (0444)");
        // A restored instance of a later run: the session's file as restore()
        // gives it, then saved again.
        StatePaths sp((scratch / "k").string(), s.string(), small);
        char* full = sp.absolute_path(rel.c_str());
        check(full && saved == fs::path(full), "restored from the session's Plugin Files/");
        check(save(sp, s, {saved}, listed, error) && listed.size() == 1 && listed[0].path == rel,
              "unchanged: named by its own path");
        // The looper writes its loop back in place: same size, other bytes.
        fs::permissions(saved, fs::perms::owner_write, fs::perm_options::add);
        { std::ofstream(saved, std::ios::binary | std::ios::trunc) << std::string(64, 'L'); }
        fs::last_write_time(saved, fs::last_write_time(saved) + std::chrono::seconds(5));
        sp.begin_save(s.string(), true);
        sp.free_path(sp.abstract_path(full));
        check(!sp.end_save(listed, error) && contains(error, "after the take"),
              "changed, during a take: not read, the save waits for after the take");
        check(save(sp, s, {saved}, listed, error) && listed.size() == 1 && listed[0].path != rel &&
                  listed[0].bytes == 64,
              "changed: hashed again and saved as the new bytes, not named by the old sha");
        std::ifstream back(s / (listed.empty() ? rel : listed[0].path), std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
        check(got == std::string(64, 'L'), "  the new file holds the new bytes");
        sp.free_path(full);
    }

    std::printf("state_paths: an absolute path a restored state held is not copied in (#276 review N5)\n");
    {
        write_file(base / "home" / "secret", 32, 's');
        StatePaths sp((scratch / "l").string(), session.string(), small);
        const std::string abs = (base / "home" / "secret").string();
        char* back = sp.absolute_path(abs.c_str());
        check(back && abs == back, "restore: returned as it is (states from before wire 4)");
        sp.free_path(back);
        std::vector<pc::StateFile> listed;
        std::string error;
        sp.begin_save(session.string(), false);
        char* named = sp.abstract_path(abs.c_str());
        check(named && abs == named, "save: named as given");
        sp.free_path(named);
        check(sp.end_save(listed, error) && listed.empty(), "  and nothing copied into Plugin Files/");
    }

    // #289 review B1: a received session whose Plugin Files/ leads out
    // through a link, at the file, at <sha256>/ or at Plugin Files/ itself.
    // Restoring must not hand the plugin that path, and a save given it anyway
    // must not copy what it reaches (a key, here a sentinel) into the session.
    std::printf("state_paths: a received Plugin Files/ that leads out through a link (#289 review B1)\n");
    {
        const std::string secret(48, 'S');
        sonora::Sha256 h;
        h.update(secret.data(), secret.size());
        const std::string sha = h.hex();
        const std::string rel = "Plugin Files/" + sha + "/kit.wav";
        write_file(base / "victim" / "id_rsa", secret.size(), 'S');
        write_file(base / "victim_dir" / "kit.wav", secret.size(), 'S');
        write_file(base / "victim_pf" / sha / "kit.wav", secret.size(), 'S');
        // Regular files under `dir`, not counting links or what they reach.
        const auto real_files = [](const fs::path& dir) {
            std::size_t n = 0;
            std::error_code e;
            for (const auto& entry : fs::recursive_directory_iterator(dir, e))
                n += fs::is_regular_file(entry.symlink_status()) ? 1 : 0;
            return n;
        };
        struct Case {
            const char* what;
            fs::path session;
        };
        const Case cases[] = {{"the file is a link", base / "R1"},
                              {"<sha256>/ is a link", base / "R2"},
                              {"Plugin Files/ is a link", base / "R3"}};
        fs::create_directories(cases[0].session / "Plugin Files" / sha);
        fs::create_symlink(base / "victim" / "id_rsa", cases[0].session / rel);
        fs::create_directories(cases[1].session / "Plugin Files");
        fs::create_directory_symlink(base / "victim_dir", cases[1].session / "Plugin Files" / sha);
        fs::create_directories(cases[2].session);
        fs::create_directory_symlink(base / "victim_pf", cases[2].session / "Plugin Files");
        for (const Case& c : cases) {
            std::printf("  case: %s\n", c.what);
            StatePaths sp((scratch / "n").string(), c.session.string(), small);
            char* got = sp.absolute_path(rel.c_str());
            check(!got && sp.refused() == 1, "    restore: the path is refused (NULL) and counted");
            if (sp.refused()) std::printf("    text: %s\n", sp.refusal_text().c_str());
            sp.free_path(got);
            // A plugin that rebuilt the path by itself and hands it to save().
            std::vector<pc::StateFile> listed;
            std::string error;
            sp.begin_save(c.session.string(), false);
            sp.free_path(sp.abstract_path((c.session / rel).c_str()));
            check(!sp.end_save(listed, error) && listed.empty(), "    save: fails, naming nothing");
            std::printf("    text: %s\n", error.c_str());
            check(real_files(c.session) == 0, "    nothing it reaches is copied into the session");
        }
    }

    std::printf("state_paths: stale .incoming-* swept at session open (#276 review N2)\n");
    {
        const fs::path s = base / "S4", pf = s / "Plugin Files";
        fs::create_directories(pf);
        const pid_t child = ::fork();
        if (child == 0) ::_exit(0);
        ::waitpid(child, nullptr, 0);
        const std::string dead = ".incoming-" + std::to_string(child) + "-1";
        const std::string live = ".incoming-" + std::to_string(::getppid()) + "-1";
        const std::string dead_link = ".incoming-" + std::to_string(child) + "-2";
        write_file(pf / dead, 10, 'd');
        write_file(pf / live, 10, 'l');
        write_file(pf / ".incoming-notes", 10, 'n');
        write_file(base / "keep" / "target", 10, 't');
        fs::create_symlink(base / "keep" / "target", pf / dead_link);
        StatePaths sp((scratch / "m").string(), s.string(), small);
        check(!fs::exists(pf / dead), "a dead host's .incoming-* is removed");
        check(fs::exists(pf / live), "a running host's is kept: its copy is in progress");
        check(fs::exists(pf / ".incoming-notes"), "a name that is not <pid>-<n> is kept");
        check(fs::is_symlink(fs::symlink_status(pf / dead_link)) && fs::exists(base / "keep" / "target"),
              "a link with a dead pid's name is not followed or removed");
    }

    std::printf("state_paths: a saved file that verifies is kept, not renamed over\n");
    {
        room = uint64_t{1} << 40;
        ::alarm(30);  // a blocked open on a FIFO fails the test instead of hanging it
        const fs::path src = scratch / "k" / "loop.bin", s5 = base / "S5";
        fs::create_directories(s5);
        write_file(src, 300, 'k');
        StatePaths sp((scratch / "k2").string(), s5.string(), small);
        std::vector<pc::StateFile> listed;
        std::string error;
        check(save(sp, s5, {src}, listed, error) && listed.size() == 1, "first save copies the file");
        const fs::path saved = s5 / listed[0].path;
        struct stat before {};
        ::stat(saved.c_str(), &before);
        StatePaths again((scratch / "k3").string(), s5.string(), small);
        check(save(again, s5, {src}, listed, error) && listed.size() == 1, "the same bytes saved again");
        struct stat after {};
        ::stat(saved.c_str(), &after);
        check(before.st_ino == after.st_ino, "  the existing file keeps its inode");
        check(no_incoming(s5), "  the temporary copy is removed");

        fs::permissions(saved, fs::perms::owner_write, fs::perm_options::add);
        { std::ofstream(saved, std::ios::binary | std::ios::trunc) << std::string(300, 'x'); }
        check(save(again, s5, {src}, listed, error) && fs::file_size(saved) == 300,
              "a file with other bytes is replaced");
        std::ifstream in(saved, std::ios::binary);
        const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(got == std::string(300, 'k'), "  by the bytes that were saved");

        fs::remove(saved);
        check(::mkfifo(saved.c_str(), 0644) == 0, "a FIFO takes the name");
        check(save(again, s5, {src}, listed, error) && listed.size() == 1, "  the save does not block on it");
        check(fs::is_regular_file(saved) && fs::file_size(saved) == 300, "  and replaces it with the file");
        ::alarm(0);
    }

    fs::remove_all(base, ec);
    std::printf(g_failed ? "state_paths: %d FAILED\n" : "state_paths: all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
