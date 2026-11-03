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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

    fs::remove_all(base, ec);
    std::printf(g_failed ? "state_paths: %d FAILED\n" : "state_paths: all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
