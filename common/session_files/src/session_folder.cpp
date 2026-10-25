// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session_files/session_folder.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>

#include "sonora/model/session_file.h"
#include "sonora/platform/file.h"
#include "sonora/sha256.h"

namespace sonora::session_files {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxSessionBytes = std::uintmax_t{256} << 20;  // 60x the spec's large session
// A 16 M-entry session is about 100 MiB of sequence files in all (ADR-0015
// §3), so one file past this is not one of ours.
constexpr std::uintmax_t kMaxSequenceBytes = std::uintmax_t{1} << 30;
// contracts/plugin's kMaxStateBytes, which this layer does not include: a
// file past it is not a state the engine wrote.
constexpr std::uintmax_t kMaxStateBytes = std::uintmax_t{16} << 20;

bool read_file(const fs::path& path, std::uintmax_t max_bytes, std::string& out) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > max_bytes) return false;
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) return false;
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    const bool ok = got == out.size() && std::fgetc(f) == EOF && !std::ferror(f);
    std::fclose(f);
    return ok;
}

// `bytes` to `tmp`, synced, then renamed over `target`. The name `target`
// is only ever given to bytes that are on the medium.
bool write_synced(const fs::path& target, const std::string& bytes, std::string& why) {
    const fs::path tmp = target.string() + ".tmp";
    std::error_code ec;
    // An earlier attempt's leftover: never named by any session file.
    fs::remove(tmp, ec);
    std::FILE* f = platform::create_exclusive(tmp.string());
    if (f == nullptr) {
        why = "cannot write " + tmp.string();
        return false;
    }
    const bool written = (bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()) &&
                         platform::sync_data(f);
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed) {
        fs::remove(tmp, ec);
        why = "writing " + target.string() + " failed";
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        why = "saving " + target.string() + " failed: " + ec.message();
        return false;
    }
    return true;
}

// session-format.md, "Versioning": before the first save over a file of an
// older format version, the original is copied to "Session File
// Backups/<Name>.v<old>.sonora" and never rotated out, so the build it came
// from (an A/B rollback) can still open it. A save that cannot keep it does
// not overwrite it either.
bool keep_older_version(const fs::path& file, std::string& why) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    std::string old;
    if (fs::file_size(file, ec) > kMaxSessionBytes && !ec) return true;
    if (!read_file(file, kMaxSessionBytes, old)) {
        why = "cannot read " + file.string() + " to keep a copy of it";
        return false;
    }
    std::int64_t version = 0;
    if (!model::file_format_version(old, version) || version == model::kFormatVersion) return true;
    // Forward compatibility: a newer build's file is never overwritten by an
    // older one, which cannot even read what it would destroy.
    if (version > model::kFormatVersion) {
        why = file.string() + " is a session file of version " + std::to_string(version) +
              ", newer than the version " + std::to_string(model::kFormatVersion) +
              " this build writes; it is left as it is. Save under another name";
        return false;
    }
    const fs::path backups = file.parent_path() / kBackupsDir;
    const fs::path copy =
        backups / (file.stem().string() + ".v" + std::to_string(version) + ".sonora");
    // The first save over it kept it; that copy is the one to keep.
    if (fs::exists(copy, ec)) return true;
    const bool created = !fs::exists(backups, ec);
    fs::create_directories(backups, ec);
    if (ec) {
        why = "cannot create " + backups.string() + " to keep the version " +
              std::to_string(version) + " file";
        return false;
    }
    if (created && !platform::sync_directory(file.parent_path().string())) {
        why = "cannot sync the session folder after creating " + backups.string();
        return false;
    }
    if (!write_synced(copy, old, why)) return false;
    if (!platform::sync_directory(backups.string())) {
        why = "cannot sync " + backups.string() + " after keeping the version " +
              std::to_string(version) + " file";
        return false;
    }
    return true;
}

// "<64 hex>.msq": a name a saver gave, the only kind cleanup may touch.
bool sequence_file_name(const std::string& name, std::string& sha) {
    if (name.size() != 64 + 4 || name.compare(64, 4, ".msq") != 0) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    sha = name.substr(0, 64);
    return true;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

}  // namespace

std::string session_folder_of(const std::string& session_file) {
    const fs::path dir = fs::path(session_file).parent_path();
    if (dir.filename() == kBackupsDir) return dir.parent_path().string();
    return dir.string();
}

SessionFolder::SessionFolder(SyncDirectory sync_directory)
    : sync_directory_(sync_directory != nullptr ? sync_directory : platform::sync_directory) {}

void SessionFolder::enter(const std::string& folder) {
    if (folder == folder_) return;
    folder_ = folder;
    files_.clear();
    known_.clear();
    std::error_code ec;
    // Interrupted saves: no session file names a ".msq.tmp".
    for (fs::directory_iterator it(fs::path(folder_) / kSequenceDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (ends_with(name, ".msq.tmp")) {
            std::error_code rm;
            fs::remove(it->path(), rm);
        }
    }
    refresh_kept();
}

// Each kept file is parsed once, when it is first seen. One that has gone
// since is dropped, so a deleted unreadable backup stops blocking cleanup.
void SessionFolder::refresh_kept() {
    std::set<std::string> present;
    for (const fs::path& dir : {fs::path(folder_), fs::path(folder_) / kBackupsDir}) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type;
            if (it->path().extension() == ".sonora" && it->is_regular_file(type)) {
                present.insert(it->path().string());
            }
        }
    }
    for (auto it = known_.begin(); it != known_.end();) {
        if (present.count(*it) != 0) {
            ++it;
            continue;
        }
        files_.dropped(*it);
        it = known_.erase(it);
    }
    for (const std::string& path : present) {
        if (!known_.insert(path).second) continue;
        ++kept_reads_;
        std::string bytes, why;
        std::set<std::string> refs;
        if (!read_file(path, kMaxSessionBytes, bytes) ||
            !model::sequence_references(bytes, refs, why)) {
            files_.kept_unreadable(path);
            continue;
        }
        files_.kept(path, std::move(refs));
    }
}

bool SessionFolder::verified_on_disk(const std::string& sha) {
    std::string bytes;
    const fs::path file = fs::path(folder_) / kSequenceDir / (sha + ".msq");
    std::error_code ec;
    if (!fs::exists(file, ec)) return false;
    return read_file(file, kMaxSequenceBytes, bytes) && sha256_hex(bytes) == sha;
}

bool SessionFolder::save(const std::string& session_file, const std::string& bytes,
                         const model::Snapshot& document, std::string& why, std::string& caution) {
    caution.clear();
    const fs::path file(session_file);
    if (!file.is_absolute() || file.extension() != ".sonora" || file.stem().empty()) {
        why = "a session file is an absolute path ending in \".sonora\", not \"" + session_file + "\"";
        return false;
    }
    std::set<std::string> refs;
    std::string parse_why;
    if (!model::sequence_references(bytes, refs, parse_why)) {
        why = "what was to be saved is not a session file: " + parse_why;
        return false;
    }

    // What to write is settled before anything on disk changes.
    std::map<std::string, model::NamedSequence> held;
    for (model::NamedSequence& n : model::named_sequences(document, digests_)) {
        const std::string sha = n.ref.sha256;
        held.emplace(sha, std::move(n));
    }
    const fs::path folder = session_folder_of(file.string());
    enter(folder.string());
    std::vector<model::NamedSequence*> todo;
    for (const std::string& sha : refs) {
        const auto it = held.find(sha);
        if (it == held.end()) {
            why = "the session names MIDI data the engine does not hold (" + sha.substr(0, 12) +
                  "...), perhaps from an edit it refused; save again";
            return false;
        }
        // A sequence whose file was missing at open (O2) is named, not
        // written: nothing here holds its notes.
        if (it->second.data->missing || files_.on_disk(sha)) continue;
        if (verified_on_disk(sha)) {
            files_.wrote(sha);
            continue;
        }
        todo.push_back(&it->second);
    }

    std::error_code ec;
    const bool created = !fs::exists(folder, ec);
    fs::create_directories(folder, ec);
    if (ec) {
        why = "cannot create the session folder " + folder.string();
        return false;
    }
    // Everything in a folder this call made is this call's, so a failed
    // save takes it all back and a retry is not pushed to "<Name> 2".
    const auto fail = [&](std::string reason) {
        if (created) {
            std::error_code rm;
            fs::remove_all(folder, rm);
            folder_.clear();
        }
        why = std::move(reason);
        return false;
    };
    // A new folder's own entry is only durable once its parent is synced.
    if (created && !sync_directory_(folder.parent_path().string())) {
        return fail("cannot sync the sessions folder after creating " + folder.string());
    }
    std::string step_why;
    if (!keep_older_version(file, step_why)) return fail(step_why);

    if (!todo.empty()) {
        const fs::path dir = folder / kSequenceDir;
        const bool made_dir = !fs::exists(dir, ec);
        fs::create_directories(dir, ec);
        if (ec) return fail("cannot create " + dir.string());
        if (made_dir && !sync_directory_(folder.string())) {
            return fail("cannot sync the session folder after creating " + dir.string());
        }
        std::vector<std::string> renamed;
        for (model::NamedSequence* n : todo) {
            if (n->encoded.empty()) n->encoded = model::encode_sequence(*n->data);
            // The name is the hash: a file torn by a crash must never carry it.
            if (!write_synced(dir / (n->ref.sha256 + ".msq"), n->encoded, step_why)) {
                return fail(step_why);
            }
            renamed.push_back(n->ref.sha256);
            ++sequences_written_;
        }
        // One sync for the batch, before any session file names what it holds.
        if (!sync_directory_(dir.string())) {
            return fail("cannot sync " + dir.string() + " after writing its sequence files");
        }
        for (const std::string& sha : renamed) files_.wrote(sha);
    }

    if (!write_synced(file, bytes, step_why)) return fail(step_why);
    // The rename has landed: the file holds the new session, so this is a
    // save. Reporting it as a failure would make a first save retry into
    // "<Name> 2" next to the file it just wrote.
    // The directory the rename landed in: "Session File Backups/" for a
    // file opened from there.
    if (!sync_directory_(file.parent_path().string())) {
        caution = "the folder could not be synced; a power cut now could lose this save";
    }
    // What the file names is what `bytes` names: it is not read back.
    files_.kept(file.string(), std::move(refs));
    known_.insert(file.string());
    // A save may have kept a pre-migration copy, which is a kept file too.
    refresh_kept();
    return true;
}

bool SessionFolder::write_states(const std::string& folder, const std::vector<StateFile>& states,
                                 std::string& why) {
    if (states.empty()) return true;
    const fs::path dir = fs::path(folder) / kPluginStateDir;
    std::error_code ec;
    const bool made_dir = !fs::exists(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        why = "cannot create " + dir.string();
        return false;
    }
    if (made_dir && !sync_directory_(folder)) {
        why = "cannot sync the session folder after creating " + dir.string();
        return false;
    }
    bool wrote = false;
    for (const StateFile& s : states) {
        // The name is the hash: bytes that are not the digest's must never
        // carry it, or the check at open refuses a state we wrote.
        if (s.bytes.size() > kMaxStateBytes || sha256_hex(s.bytes) != s.sha256) {
            why = "a plugin state does not match its digest " + s.sha256;
            return false;
        }
        const fs::path file = fs::path(folder) / model::plugin_state_file(s.sha256);
        std::string there;
        if (fs::exists(file, ec) && read_file(file, kMaxStateBytes, there) && there == s.bytes) continue;
        if (!write_synced(file, s.bytes, why)) return false;
        wrote = true;
        ++states_written_;
    }
    if (wrote && !sync_directory_(dir.string())) {
        why = "cannot sync " + dir.string() + " after writing plugin states";
        return false;
    }
    return true;
}

int SessionFolder::clean(std::string& caution) {
    if (folder_.empty()) return 0;
    refresh_kept();
    const std::vector<std::string> unreadable = files_.unreadable();
    if (!unreadable.empty()) {
        caution = "no sequence file is cleaned up while " + unreadable.front() +
                  " cannot be read: what it names is unknown";
        return 0;
    }
    const fs::path dir = fs::path(folder_) / kSequenceDir;
    std::vector<std::string> present;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string sha;
        if (sequence_file_name(it->path().filename().string(), sha)) present.push_back(sha);
    }
    int gone = 0;
    for (const std::string& sha : files_.deletable(present)) {
        // A crash here leaves an extra file, never a missing one.
        std::error_code rm;
        if (fs::remove(dir / (sha + ".msq"), rm)) {
            files_.deleted(sha);
            ++gone;
        }
    }
    return gone;
}

}  // namespace sonora::session_files
