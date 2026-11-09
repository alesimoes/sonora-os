// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data's copier (ADR-0123 decision 3): the plan's files come
// into "Audio Files/" as hard links or as synced copies, and a failure or a
// cancel at any file leaves the destination as it was, the other session's
// files untouched. Cancel is driven from the progress callback, so no test
// waits on a clock.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "sonora/session_files/session_import_copy.h"

namespace fs = std::filesystem;
namespace sf = sonora::session_files;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-session-import-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
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

// A source session with three files and the plan that brings them in.
struct Setup {
    TempDir tmp;
    fs::path source = tmp.path / "Source";
    fs::path dest = tmp.path / "Dest";
    sf::ImportPlan plan;
    Setup() {
        fs::create_directories(dest);
        for (int i = 1; i <= 3; ++i) {
            const std::string name = "take" + std::to_string(i) + ".wav";
            write(source / "Audio Files" / name, std::string(3000 * i, static_cast<char>('a' + i)));
            sf::FileCopy c;
            c.source_path = (source / "Audio Files" / name).string();
            c.dest_path = "Audio Files/" + name;
            c.source = static_cast<sonora::model::Id>(10 + i);
            plan.copies.push_back(c);
        }
    }
    // What is in the destination's Audio Files/, by name.
    std::vector<std::string> landed() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dest / "Audio Files", ec)) {
            names.push_back(e.path().filename().string());
        }
        return names;
    }
    bool sources_intact() const {
        for (int i = 1; i <= 3; ++i) {
            const fs::path p = source / "Audio Files" / ("take" + std::to_string(i) + ".wav");
            if (read(p) != std::string(3000 * i, static_cast<char>('a' + i))) return false;
        }
        return true;
    }
};

int links_on_one_filesystem() {
    Setup s;
    std::atomic<bool> cancel{false};
    std::vector<sf::CopyProgress> seen;
    sf::CopyReport report;
    const sf::CopyOutcome out = sf::perform_copies(
        s.plan, s.dest.string(), cancel, [&](const sf::CopyProgress& p) { seen.push_back(p); },
        report);
    CHECK(out == sf::CopyOutcome::Done && report.outcome == sf::CopyOutcome::Done);
    CHECK(report.linked == 3 && report.copied == 0);
    for (const sf::FileCopy& c : s.plan.copies) {
        CHECK(read(s.dest / c.dest_path) == read(c.source_path));
        CHECK(fs::hard_link_count(s.dest / c.dest_path) == 2);
    }
    CHECK(!seen.empty() && seen.back().files_done == 3 && seen.back().files_total == 3);
    CHECK(seen.back().bytes_done == seen.back().bytes_total && seen.back().bytes_total == 18000);
    return 0;
}

int copies_when_asked_to_and_leaves_no_part() {
    Setup s;
    std::atomic<bool> cancel{false};
    sf::CopyReport report;
    sf::CopyOptions options;
    options.hard_links = false;
    options.chunk_bytes = 1024;
    CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, {}, report, options) ==
          sf::CopyOutcome::Done);
    CHECK(report.copied == 3 && report.linked == 0);
    for (const sf::FileCopy& c : s.plan.copies) {
        CHECK(read(s.dest / c.dest_path) == read(c.source_path));
        CHECK(fs::hard_link_count(s.dest / c.dest_path) == 1);
    }
    CHECK(s.landed().size() == 3);   // no ".part" left
    return 0;
}

int a_missing_source_writes_nothing() {
    Setup s;
    fs::remove(s.plan.copies[2].source_path);
    std::atomic<bool> cancel{false};
    sf::CopyReport report;
    CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, {}, report) ==
          sf::CopyOutcome::Failed);
    CHECK(report.file == s.plan.copies[2].source_path);
    CHECK(report.message.find("take3.wav") != std::string::npos);
    CHECK(s.landed().empty());
    return 0;
}

int a_symlinked_source_is_refused() {
    Setup s;
    const fs::path secret = s.tmp.path / "secret";
    write(secret, "not audio");
    fs::remove(s.plan.copies[1].source_path);
    fs::create_symlink(secret, s.plan.copies[1].source_path);
    std::atomic<bool> cancel{false};
    sf::CopyReport report;
    CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, {}, report) ==
          sf::CopyOutcome::Failed);
    CHECK(report.file == s.plan.copies[1].source_path && s.landed().empty());
    return 0;
}

int a_taken_name_is_never_overwritten() {
    Setup s;
    write(s.dest / "Audio Files" / "take2.wav", "the session's own take");
    std::atomic<bool> cancel{false};
    sf::CopyReport report;
    CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, {}, report) ==
          sf::CopyOutcome::Failed);
    CHECK(report.message.find("nothing is overwritten") != std::string::npos);
    CHECK(read(s.dest / "Audio Files" / "take2.wav") == "the session's own take");
    CHECK(s.landed().size() == 1);
    s.plan.copies[0].dest_path = "Audio Files/../escape.wav";
    CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, {}, report) ==
          sf::CopyOutcome::Failed);
    CHECK(!fs::exists(s.dest / "escape.wav") && !fs::exists(s.tmp.path / "escape.wav"));
    return 0;
}

// The third name is taken after the check, by a file the progress callback
// writes once two files are in: the import fails, the two it brought are
// deleted, and the file it did not write is left as it is.
int a_failure_part_way_takes_back_what_it_wrote() {
    for (const bool links : {true, false}) {
        Setup s;
        std::atomic<bool> cancel{false};
        sf::CopyReport report;
        sf::CopyOptions options;
        options.hard_links = links;
        const fs::path taken = s.dest / "Audio Files" / "take3.wav";
        const auto progress = [&](const sf::CopyProgress& p) {
            if (p.files_done == 2 && !fs::exists(taken)) write(taken, "someone else's");
        };
        CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, progress, report, options) ==
              sf::CopyOutcome::Failed);
        CHECK(report.file == s.plan.copies[2].source_path);
        CHECK(s.landed() == std::vector<std::string>{"take3.wav"});
        CHECK(read(taken) == "someone else's");
        CHECK(s.sources_intact());
    }
    return 0;
}

int a_cancel_takes_back_what_it_wrote() {
    for (const bool links : {true, false}) {
        Setup s;
        std::atomic<bool> cancel{false};
        sf::CopyReport report;
        sf::CopyOptions options;
        options.hard_links = links;
        options.chunk_bytes = 1000;
        // Two files in, and part of the third when copying.
        const auto progress = [&](const sf::CopyProgress& p) {
            if (p.files_done == 2 && (links || p.bytes_done > 9000 + 1000)) cancel.store(true);
        };
        CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, progress, report, options) ==
              sf::CopyOutcome::Cancelled);
        CHECK(report.outcome == sf::CopyOutcome::Cancelled && report.file.empty());
        CHECK(s.landed().empty());
        CHECK(s.sources_intact());
    }
    return 0;
}

int a_cancel_before_the_first_file_writes_nothing() {
    Setup s;
    std::atomic<bool> cancel{true};
    sf::CopyReport report;
    CHECK(sf::perform_copies(s.plan, s.dest.string(), cancel, {}, report) ==
          sf::CopyOutcome::Cancelled);
    CHECK(s.landed().empty());
    return 0;
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<int()>>> tests = {
        {"links_on_one_filesystem", links_on_one_filesystem},
        {"copies_when_asked_to_and_leaves_no_part", copies_when_asked_to_and_leaves_no_part},
        {"a_missing_source_writes_nothing", a_missing_source_writes_nothing},
        {"a_symlinked_source_is_refused", a_symlinked_source_is_refused},
        {"a_taken_name_is_never_overwritten", a_taken_name_is_never_overwritten},
        {"a_failure_part_way_takes_back_what_it_wrote", a_failure_part_way_takes_back_what_it_wrote},
        {"a_cancel_takes_back_what_it_wrote", a_cancel_takes_back_what_it_wrote},
        {"a_cancel_before_the_first_file_writes_nothing",
         a_cancel_before_the_first_file_writes_nothing},
    };
    int failed = 0;
    for (const auto& t : tests) {
        if (t.second() != 0) {
            std::fprintf(stderr, "FAILED: %s\n", t.first);
            ++failed;
        }
    }
    if (failed != 0) return 1;
    std::printf("session_import_copy: %zu tests ok\n", tests.size());
    return 0;
}
