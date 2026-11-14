// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data's copier (ADR-0123 decision 3). See the header for
// the all-or-nothing rule; this file is how it is kept.
#include "sonora/session_files/session_import_copy.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include "sonora/platform/file.h"
#include "sonora/session_files/session_folder.h"

namespace sonora::session_files {

namespace fs = std::filesystem;

namespace {

// "Audio Files/<one basename>": the planner never names anything else, and
// a plan that does was not made by it.
bool audio_path(const std::string& rel) {
    const std::string prefix = std::string(kAudioDir) + "/";
    if (rel.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string name = rel.substr(prefix.size());
    if (name.empty() || name == "." || name == "..") return false;
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == '\0') return false;
    }
    return true;
}

bool exists_here(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

class Copier {
public:
    Copier(const fs::path& folder, const std::atomic<bool>& cancel, const CopyProgressFn& progress,
           CopyReport& report, const CopyOptions& options)
        : folder_(folder), cancel_(cancel), progress_(progress), report_(report),
          options_(options) {}

    ~Copier() { take_back(); }

    bool check(const std::vector<FileCopy>& copies);
    bool bring(const FileCopy& c);
    bool finish();
    bool cancelled() {
        if (!cancel_.load(std::memory_order_acquire)) return false;
        report_.outcome = CopyOutcome::Cancelled;
        report_.message = "Import Session Data cancelled; nothing was imported";
        report_.file.clear();
        return true;
    }

private:
    bool fail(const std::string& message, const std::string& file) {
        report_.outcome = CopyOutcome::Failed;
        report_.message = message;
        report_.file = file;
        return false;
    }
    void tell() {
        if (progress_) progress_(done_);
    }
    bool copy_durably(const fs::path& from, const fs::path& to);
    void take_back();

    fs::path folder_;
    const std::atomic<bool>& cancel_;
    const CopyProgressFn& progress_;
    CopyReport& report_;
    const CopyOptions& options_;
    CopyProgress done_;
    std::vector<fs::path> written_;
    bool kept_ = false;
};

// Every file before any is written: a missing source or a taken name found
// at file 30 would otherwise cost 29 copies and their deletion.
bool Copier::check(const std::vector<FileCopy>& copies) {
    std::set<std::string> targets;
    done_.files_total = copies.size();
    for (const FileCopy& c : copies) {
        if (!audio_path(c.dest_path)) {
            return fail("Import Session Data: \"" + c.dest_path + "\" is not a file of Audio Files/",
                        c.source_path);
        }
        if (!targets.insert(c.dest_path).second) {
            return fail("Import Session Data: two files would both be \"" + c.dest_path + "\"",
                        c.source_path);
        }
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(c.source_path, ec);
        // Never through a symlink: a hostile session could point one at any
        // file this user can read.
        if (ec || !fs::is_regular_file(st)) {
            return fail("Import Session Data: cannot read \"" + c.source_path + "\"", c.source_path);
        }
        const std::uintmax_t size = fs::file_size(c.source_path, ec);
        if (ec) {
            return fail("Import Session Data: cannot read \"" + c.source_path + "\"", c.source_path);
        }
        done_.bytes_total += size;
        if (exists_here(folder_ / c.dest_path)) {
            return fail("Import Session Data: \"" + c.dest_path +
                            "\" is already in this session; nothing is overwritten",
                        c.source_path);
        }
    }
    std::error_code ec;
    fs::create_directories(folder_ / kAudioDir, ec);
    if (ec) return fail("Import Session Data: cannot create Audio Files/ in the session", {});
    tell();
    return true;
}

bool Copier::bring(const FileCopy& c) {
    const fs::path to = folder_ / c.dest_path;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(c.source_path, ec);
    std::string why;
    if (options_.hard_links && platform::hard_link(c.source_path, to.string(), why)) {
        written_.push_back(to);
        ++report_.linked;
        done_.bytes_done += size;
    } else {
        if (!copy_durably(c.source_path, to)) return false;
        ++report_.copied;
    }
    ++done_.files_done;
    tell();
    return true;
}

// Through "<name>.part", which an interrupted run leaves for
// sweep_partial_copies() to find, then to its name without replacing one.
bool Copier::copy_durably(const fs::path& from, const fs::path& to) {
    const fs::path part = to.string() + ".part";
    std::FILE* in = std::fopen(from.string().c_str(), "rb");
    if (in == nullptr) return fail("Import Session Data: cannot read \"" + from.string() + "\"", from.string());
    std::FILE* out = platform::create_exclusive(part.string());
    if (out == nullptr) {
        std::fclose(in);
        return fail("Import Session Data: cannot write \"" + part.string() + "\"", from.string());
    }
    written_.push_back(part);
    std::vector<char> buffer(options_.chunk_bytes > 0 ? options_.chunk_bytes : 1);
    bool ok = true;
    bool stopped = false;
    for (;;) {
        if (cancelled()) {
            stopped = true;
            break;
        }
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), in);
        if (got > 0 && std::fwrite(buffer.data(), 1, got, out) != got) ok = false;
        done_.bytes_done += got;
        if (got < buffer.size() || !ok) break;
        tell();
    }
    ok = ok && !std::ferror(in) && (stopped || platform::sync_data(out));
    std::fclose(in);
    ok = std::fclose(out) == 0 && ok;
    if (stopped) return false;
    if (!ok) return fail("Import Session Data: copying \"" + from.string() + "\" failed", from.string());
    std::string why;
    std::error_code ec;
    if (platform::hard_link(part.string(), to.string(), why)) {
        written_.push_back(to);
        fs::remove(part, ec);
    } else if (exists_here(to)) {
        return fail("Import Session Data: \"" + to.string() + "\" appeared during the import",
                    from.string());
    } else {
        // No hard links on this filesystem (exFAT): checked free just above.
        fs::rename(part, to, ec);
        if (ec) return fail("Import Session Data: cannot name \"" + to.string() + "\"", from.string());
        written_.back() = to;
    }
    return true;
}

bool Copier::finish() {
    if (!platform::sync_directory((folder_ / kAudioDir).string())) {
        return fail("Import Session Data: cannot sync Audio Files/; nothing was imported", {});
    }
    kept_ = true;
    return true;
}

// Removes names only: a hard link's removal leaves the other session's file
// as it was.
void Copier::take_back() {
    if (kept_ || written_.empty()) return;
    for (const fs::path& p : written_) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    (void)platform::sync_directory((folder_ / kAudioDir).string());
    written_.clear();
}

}  // namespace

CopyOutcome perform_copies(const ImportPlan& plan, const std::string& session_folder,
                           const std::atomic<bool>& cancel, const CopyProgressFn& progress,
                           CopyReport& report, const CopyOptions& options) {
    report = CopyReport{};
    Copier copier(fs::path(session_folder), cancel, progress, report, options);
    if (!copier.check(plan.copies)) return report.outcome;
    for (const FileCopy& c : plan.copies) {
        if (copier.cancelled() || !copier.bring(c)) return report.outcome;
    }
    copier.finish();
    return report.outcome;
}

}  // namespace sonora::session_files
