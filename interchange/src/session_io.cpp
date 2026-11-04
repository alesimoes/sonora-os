// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sonora/interchange/interchange.h"
#include "sonora/model/session_file.h"
#include "sonora/model/sequence_file.h"
#include "sonora/session_files/session_folder.h"

namespace sonora::interchange {

namespace fs = std::filesystem;

namespace {

// A session file or a sequence file is never larger than this; a converter
// handed more is handed something else.
constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{1} << 30;

bool slurp(const fs::path& path, std::string& bytes, std::string& why) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        why = "the file is not there";
        return false;
    }
    if (fs::file_size(path, ec) > kMaxFileBytes) {
        why = "it is too large to be a session file";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        why = "it cannot be read";
        return false;
    }
    std::ostringstream s;
    s << in.rdbuf();
    bytes = s.str();
    return true;
}

class FolderSequences : public model::SequenceSource {
public:
    explicit FolderSequences(fs::path folder) : folder_(std::move(folder)) {}
    bool read(const std::string& file, std::string& bytes, std::string& why) override {
        // The file name comes from the document; it is a path under the
        // folder or it is refused, never a way out of it.
        const fs::path rel = fs::path(file).lexically_normal();
        if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
            why = "its name leaves the session folder";
            return false;
        }
        return slurp(folder_ / rel, bytes, why);
    }

private:
    fs::path folder_;
};

}  // namespace

bool read_session(const std::string& path, model::Snapshot& out, ConversionReport& report,
                  std::string& error) {
    std::string bytes, why;
    if (!slurp(fs::path(path), bytes, why)) {
        error = path + ": " + why;
        return false;
    }
    FolderSequences sequences(fs::absolute(fs::path(path)).parent_path());
    model::Loaded loaded;
    model::LoadError load_error;
    if (!model::deserialize(bytes, loaded, load_error, &sequences)) {
        error = path + ": " + load_error.text();
        return false;
    }
    for (const model::Repair& r : loaded.repairs) {
        report.approximated.push_back("repaired on open: " + r.text());
    }
    for (const model::MissingSequence& m : loaded.missing_sequences) {
        report.dropped.push_back("MIDI sequence " + m.ref.file + ": " + m.why);
    }
    out = std::move(loaded.snapshot);
    return true;
}

bool write_session(const std::string& path, const model::Snapshot& snapshot,
                   ConversionReport& report, std::string& error) {
    const fs::path absolute = fs::absolute(fs::path(path));
    if (absolute.extension() != ".sonora") {
        error = path + ": a session file ends in .sonora";
        return false;
    }
    model::WriteInfo info;
    info.written_by = "sonora-convert";
    std::string bytes;
    model::SequenceDigests digests;
    if (!model::serialize(snapshot, info, bytes, error, &digests)) return false;
    session_files::SessionFolder folder;
    std::string why, caution;
    if (!folder.save(absolute.string(), bytes, snapshot, why, caution)) {
        error = path + ": " + why;
        return false;
    }
    if (!caution.empty()) report.approximated.push_back("not made durable: " + caution);
    return true;
}

}  // namespace sonora::interchange
