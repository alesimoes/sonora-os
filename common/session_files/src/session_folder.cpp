// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session_files/session_folder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <vector>
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
    // The session's own backups folder, also for a file already in it (an
    // autosave), which would otherwise get a backups folder of its own.
    const fs::path backups = fs::path(session_folder_of(file.string())) / kBackupsDir;
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
    if (created && !platform::sync_directory(backups.parent_path().string())) {
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

// Whether `file` is there with exactly the bytes `sha` names.
bool state_on_disk(const fs::path& file, const std::string& sha) {
    std::string there;
    std::error_code ec;
    return fs::exists(file, ec) && read_file(file, kMaxStateBytes, there) && sha256_hex(there) == sha;
}

bool all_digits(const std::string& s, std::size_t from, std::size_t count) {
    if (from + count > s.size()) return false;
    for (std::size_t i = from; i < from + count; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// A rotated backup's place in the order: its stamp, then its "-<n>" (1
// without one). Names, not modification times: a copy tool resets those.
struct BackupKey {
    std::string stamp;
    unsigned long n = 1;
    bool operator<(const BackupKey& o) const { return stamp != o.stamp ? stamp < o.stamp : n < o.n; }
};

bool backup_key(const std::string& name, const std::string& stem, BackupKey& key) {
    if (!rotated_backup_name(name, stem)) return false;
    const std::size_t at = stem.size() + 1;
    key.stamp = name.substr(at, 15);
    const std::size_t rest = at + 15;
    key.n = name[rest] == '-' ? std::stoul(name.substr(rest + 1, name.size() - 7 - rest - 1)) : 1;
    return true;
}

// "yyyymmdd-hhmmss" in UTC, so names sort in the order they were written
// across daylight saving and a change of time zone. Computed here (days to
// civil date) rather than through gmtime, which is not thread-safe.
std::string utc_stamp(long long t) {
    long long days = t / 86400;
    long long secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long m = mp < 10 ? mp + 3 : mp - 9;
    long long y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    y = std::min<long long>(std::max<long long>(y, 0), 9999);
    char out[32];
    std::snprintf(out, sizeof out, "%04lld%02lld%02lld-%02lld%02lld%02lld", y, m, d, secs / 3600,
                  secs / 60 % 60, secs % 60);
    return out;
}

long long now_seconds() { return static_cast<long long>(std::time(nullptr)); }

// The path with no trailing separator, so two spellings of one folder compare.
fs::path plain(const fs::path& p) {
    fs::path n = p.lexically_normal();
    if (n.filename().empty() && n.has_parent_path()) n = n.parent_path();
    return n;
}

// Whether `inner` is `outer` or inside it, by name.
bool within(const fs::path& inner, const fs::path& outer) {
    const fs::path a = plain(inner), b = plain(outer);
    const auto m = std::mismatch(b.begin(), b.end(), a.begin(), a.end());
    return m.first == b.end();
}

// A source's path as the document holds it, if it may be copied: relative,
// inside the folder.
bool inside_relative(const std::string& path) {
    const fs::path p(path);
    if (path.empty() || p.is_absolute() || p.has_root_name()) return false;
    for (const fs::path& part : p) {
        if (part == "..") return false;
    }
    return true;
}

std::string megabytes(std::uint64_t bytes) {
    return std::to_string((bytes + (1u << 20) - 1) >> 20) + " MB";
}

}  // namespace

namespace {

// The stem a rotated backup's name was made from: "Mix" of
// "Mix.20270115-080000-2.sonora". Stems may hold dots, so each is tried.
bool rotated_backup_stem(const std::string& name, std::string& stem) {
    for (std::size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) {
        if (rotated_backup_name(name, name.substr(0, dot))) {
            stem = name.substr(0, dot);
            return true;
        }
    }
    return false;
}

}  // namespace

std::string autosave_file_of(const std::string& session_file) {
    const fs::path file(session_file);
    const fs::path folder = plain(session_folder_of(session_file));
    const std::string name = file.filename().string();
    const bool in_backups = plain(file.parent_path()).filename() == kBackupsDir;
    const char* const suffix = ".autosave.sonora";
    // An autosave opened from the backups folder autosaves onto itself.
    if (in_backups && ends_with(name, suffix) && name.size() > std::strlen(suffix)) return plain(file).string();
    std::string stem = file.stem().string();
    if (in_backups) rotated_backup_stem(name, stem);
    // Per session file: two files in one folder must not share an autosave.
    return (folder / kBackupsDir / (stem + suffix)).string();
}

bool rotated_backup_name(const std::string& name, const std::string& stem) {
    // "<stem>." + "yyyymmdd-hhmmss" + optional "-<n>" + ".sonora"
    const std::size_t at = stem.size() + 1;
    if (stem.empty() || name.size() < at + 15 + 7 || name.compare(0, stem.size(), stem) != 0 ||
        name[stem.size()] != '.' || !ends_with(name, ".sonora")) {
        return false;
    }
    if (!all_digits(name, at, 8) || name[at + 8] != '-' || !all_digits(name, at + 9, 6)) return false;
    const std::size_t rest = at + 15, end = name.size() - 7;
    if (rest == end) return true;
    // "-<n>", n from 2, no leading zero, at most 6 digits.
    if (name[rest] != '-' || end - rest < 2 || end - rest > 7 || name[rest + 1] == '0') return false;
    if (end - rest == 2 && name[rest + 1] == '1') return false;
    return all_digits(name, rest + 1, end - rest - 1);
}

namespace {

// "<media name>.part": what copy_synced() writes before its rename, for a
// file save_as() copies, which has a stem and an extension and is not
// itself a ".part" or a ".tmp". A "notes.part" of the operator's is not.
bool partial_copy_name(const std::string& name) {
    if (!ends_with(name, ".part")) return false;
    const std::string media = name.substr(0, name.size() - 5);
    if (media.empty() || ends_with(media, ".part") || ends_with(media, ".tmp")) return false;
    const fs::path p(media);
    return !p.stem().empty() && p.has_extension();
}

}  // namespace

int sweep_partial_copies(const std::string& folder) {
    std::vector<fs::path> doomed;
    for (const char* sub : {kAudioDir, kMidiFilesDir}) {
        const fs::path dir = fs::path(folder) / sub;
        std::error_code ec;
        if (!fs::is_directory(fs::symlink_status(dir, ec)) || ec) continue;
        // Recursive like save_as(), and like it never into a directory
        // symlink: its files are not the session's.
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type;
            const std::string name = it->path().filename().string();
            if (!partial_copy_name(name) || !fs::is_regular_file(it->symlink_status(type)) || type) continue;
            // The rename that ends a copy takes its ".part" away: one
            // beside its file is not a copy's.
            std::error_code there;
            const fs::path media = it->path().parent_path() / name.substr(0, name.size() - 5);
            if (fs::exists(fs::symlink_status(media, there))) continue;
            doomed.push_back(it->path());
        }
    }
    int gone = 0;
    for (const fs::path& p : doomed) {
        std::error_code rm;
        if (fs::remove(p, rm)) ++gone;
    }
    return gone;
}

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
    sweep_states(folder_);
    refresh_kept();
}

void SessionFolder::sweep_states(const std::string& folder) {
    if (!swept_.insert(folder).second) return;
    std::error_code ec;
    // Only this saver writes there, and never while it sweeps.
    for (fs::directory_iterator it(fs::path(folder) / kPluginStateDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ends_with(it->path().filename().string(), ".state.tmp")) {
            std::error_code rm;
            fs::remove(it->path(), rm);
        }
    }
}

bool SessionFolder::make_state_dir(const std::string& folder, std::string& why) {
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
    return true;
}

bool SessionFolder::sync_states(const std::string& folder, const std::vector<std::string>& paths,
                                std::string& why) {
    const bool covered = std::all_of(paths.begin(), paths.end(),
                                     [&](const std::string& p) { return durable_states_.count(p) != 0; });
    if (covered) return true;
    const fs::path dir = fs::path(folder) / kPluginStateDir;
    if (!sync_directory_(dir.string())) {
        why = "cannot sync " + dir.string() + " after writing plugin states";
        return false;
    }
    durable_states_.insert(paths.begin(), paths.end());
    return true;
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
                         const model::Snapshot& document, std::string& why, std::string& caution,
                         const std::string& state_source) {
    caution.clear();
    const fs::path file(session_file);
    if (!file.is_absolute() || file.extension() != ".sonora" || file.stem().empty()) {
        why = "a session file is an absolute path ending in \".sonora\", not \"" + session_file + "\"";
        return false;
    }
    std::string rotated_stem;
    if (plain(file.parent_path()).filename() == kBackupsDir &&
        rotated_backup_stem(file.filename().string(), rotated_stem)) {
        // Pruning deletes rotated backups, work saved into one with it.
        why = file.filename().string() + " is a backup that pruning removes; use Save As to keep this session";
        return false;
    }
    std::set<std::string> refs;
    std::string parse_why;
    std::set<std::string> state_refs;
    if (!model::sequence_references(bytes, refs, parse_why) ||
        !model::plugin_state_references(bytes, state_refs, parse_why)) {
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
            swept_.erase(folder.string());
            const std::string under = (folder / "").string();
            for (auto it = durable_states_.begin(); it != durable_states_.end();)
                it = it->compare(0, under.size(), under) == 0 ? durable_states_.erase(it) : std::next(it);
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

    if (!state_refs.empty()) {
        // The states a flush wrote sit in the engine's folder, which need
        // not be this one: a session never points outside its own folder,
        // so each is copied here before the file names it.
        if (!make_state_dir(folder.string(), step_why)) return fail(step_why);
        std::set<std::string> replaced;   // what the file being replaced names
        bool replaced_read = false;
        std::vector<std::string> placed;
        for (const std::string& sha : state_refs) {
            const fs::path target = folder / model::plugin_state_file(sha);
            if (state_on_disk(target, sha)) {
                placed.push_back(target.string());
                continue;
            }
            std::string copy;
            const fs::path source = fs::path(state_source) / model::plugin_state_file(sha);
            if (state_source.empty() || !read_file(source, kMaxStateBytes, copy) || sha256_hex(copy) != sha) {
                if (!replaced_read) {
                    replaced_read = true;
                    std::string old, ignored;
                    if (read_file(file, kMaxSessionBytes, old)) model::plugin_state_references(old, replaced, ignored);
                }
                // Lost before this save, and said at open: refusing would
                // leave a session that can never be saved again.
                if (replaced.count(sha) != 0) continue;
                return fail("the session names a plugin state (" + sha.substr(0, 12) +
                            "...) that is neither in " + (folder / kPluginStateDir).string() +
                            " nor where the engine keeps them; flush the plugins' state and save again");
            }
            if (!write_synced(target, copy, step_why)) return fail(step_why);
            ++states_written_;
            placed.push_back(target.string());
        }
        if (!sync_states(folder.string(), placed, step_why)) return fail(step_why);
    }

    // Step 5's bytes, read before the rename replaces them, which is why the
    // backup is written from memory: there is no moment without the file.
    std::string old;
    const bool rotates = backups_ > 0 && file.parent_path().filename() != kBackupsDir &&
                         fs::exists(file, ec) && read_file(file, kMaxSessionBytes, old);

    if (!write_synced(file, bytes, step_why)) return fail(step_why);
    // The rename has landed: the file holds the new session, so this is a
    // save. Reporting it as a failure would make a first save retry into
    // "<Name> 2" next to the file it just wrote.
    // The directory the rename landed in: "Session File Backups/" for a
    // file opened from there.
    if (!sync_directory_(file.parent_path().string())) {
        caution = "the folder could not be synced; a power cut now could lose this save";
    }
    if (rotates && old != bytes) keep_backup(file.string(), old, caution);
    // What the file names is what `bytes` names: it is not read back.
    files_.kept(file.string(), std::move(refs));
    known_.insert(file.string());
    // A save may have kept a pre-migration copy, which is a kept file too.
    refresh_kept();
    return true;
}

bool SessionFolder::keep_backup(const std::string& session_file, const std::string& old,
                                std::string& caution) {
    const auto say = [&caution](const std::string& line) {
        caution = caution.empty() ? line : caution + "; " + line;
        return false;
    };
    const fs::path file(session_file);
    const fs::path folder = file.parent_path();
    const fs::path dir = folder / kBackupsDir;
    const std::string stem = file.stem().string();
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(dir, ec);
    if (fs::exists(st) && !fs::is_directory(st)) {
        // A link: whatever it points to is not this session's to fill or prune.
        return say("no backup was kept: " + dir.string() + " is not a folder of the session");
    }
    if (!fs::exists(st)) {
        fs::create_directory(dir, ec);
        if (ec) return say("no backup was kept: cannot create " + dir.string());
        if (!sync_directory_(folder.string())) {
            return say("no backup was kept: cannot sync the session folder after creating " + dir.string());
        }
    }
    // The newest name there: a clock set back (a machine with no network
    // time) must not make the new backup the oldest, which pruning removes.
    BackupKey key{utc_stamp(clock_ != nullptr ? clock_() : now_seconds()), 1};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        BackupKey there;
        if (!backup_key(it->path().filename().string(), stem, there) || there < key) continue;
        key = there;
        ++key.n;
    }
    fs::path target;
    for (;; ++key.n) {
        target = dir / (stem + "." + key.stamp + (key.n > 1 ? "-" + std::to_string(key.n) : "") + ".sonora");
        std::error_code exists;
        if (!fs::exists(fs::symlink_status(target, exists))) break;
    }
    std::string why;
    if (!write_synced(target, old, why)) return say("no backup was kept: " + why);
    if (!sync_directory_(dir.string())) return say("the backup " + target.string() + " may not survive a power cut");
    std::string pruned;
    prune_backups(folder.string(), stem, backups_, pruned);
    if (!pruned.empty()) say(pruned);
    return true;
}

int SessionFolder::prune_backups(const std::string& folder, const std::string& stem, std::size_t keep,
                                 std::string& caution) {
    const fs::path dir = fs::path(folder) / kBackupsDir;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(dir, ec);
    if (!fs::exists(st)) return 0;
    if (!fs::is_directory(st)) {
        caution = "old backups are not pruned: " + dir.string() + " is not a folder of the session";
        return 0;
    }
    std::vector<std::pair<BackupKey, fs::path>> found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        BackupKey key;
        std::error_code type;
        // symlink_status: a link named as a backup is the operator's, not ours.
        if (!backup_key(it->path().filename().string(), stem, key) ||
            !fs::is_regular_file(it->symlink_status(type)) || type) {
            continue;
        }
        found.emplace_back(key, it->path());
    }
    if (found.size() <= keep) return 0;
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return b.first < a.first; });
    int gone = 0;
    for (std::size_t i = keep; i < found.size(); ++i) {
        std::error_code rm;
        if (fs::remove(found[i].second, rm)) ++gone;
    }
    return gone;
}

bool SessionFolder::autosave(const std::string& session_file, const model::Snapshot& document,
                             const std::string& written_by, const std::string& state_source,
                             std::string& why, std::string& caution) {
    caution.clear();
    const fs::path main(session_file);
    if (!main.is_absolute() || main.extension() != ".sonora") {
        why = "an autosave is of a session file's absolute path, not \"" + session_file + "\"";
        return false;
    }
    const fs::path target = autosave_file_of(session_file);
    const fs::path dir = target.parent_path();
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(dir, ec);
    if (fs::exists(st) && !fs::is_directory(st)) {
        why = dir.string() + " is not a folder of the session; the autosave is not written outside it";
        return false;
    }
    // One past the file's, so it is offered at open only until the next Save,
    // which writes that generation itself.
    std::uint64_t generation = 0;
    std::string main_bytes;
    if (read_file(main, kMaxSessionBytes, main_bytes)) model::file_save_generation(main_bytes, generation);
    std::string bytes, error;
    if (!model::serialize(document, model::WriteInfo{written_by, generation + 1}, bytes, error, &digests_)) {
        why = "the autosave could not be made: " + error;
        return false;
    }
    // The autosave of a crash holds edits the file does not; a restarted
    // saver must not be the one to replace it.
    enter(session_folder_of(session_file));
    set_aside_.clear();
    std::string there;
    const bool had = read_file(target, kMaxSessionBytes, there);
    if (had && there == bytes) {
        autosaves_written_.insert(target.string());
        return true;
    }
    if (had && autosaves_written_.count(target.string()) == 0) {
        const std::string stem = target.filename().string().substr(
            0, target.filename().string().size() - std::string(".sonora").size());
        const std::string stamp = utc_stamp(clock_ != nullptr ? clock_() : now_seconds());
        fs::path moved;
        for (unsigned n = 1;; ++n) {
            moved = dir / (stem + ".recovered-" + stamp + (n > 1 ? "-" + std::to_string(n) : "") + ".sonora");
            std::error_code exists;
            if (!fs::exists(fs::symlink_status(moved, exists))) break;
        }
        // A hard link first: the autosave stays in place until the new one
        // replaces it, so a crash here leaves one. exFAT has none, so there
        // the file is renamed.
        std::error_code mv;
        fs::create_hard_link(target, moved, mv);
        if (mv) fs::rename(target, moved, mv);
        if (mv) {
            why = "the autosave left by an earlier run could not be moved aside: " + mv.message();
            return false;
        }
        if (!sync_directory_(dir.string())) {
            why = "cannot sync " + dir.string() + " after moving the earlier autosave aside";
            return false;
        }
        set_aside_ = moved.filename().string();
    }
    if (!fs::exists(st)) {
        fs::create_directories(dir, ec);
        if (ec) {
            why = "cannot create " + dir.string() + " for the autosave";
            return false;
        }
        if (!sync_directory_(dir.parent_path().string())) {
            why = "cannot sync the session folder after creating " + dir.string();
            return false;
        }
    }
    if (!save(target.string(), bytes, document, why, caution, state_source)) return false;
    autosaves_written_.insert(target.string());
    if (!set_aside_.empty()) {
        const std::string said = "the autosave of an earlier run is kept as " + set_aside_;
        caution = caution.empty() ? said : caution + "; " + said;
    }
    return true;
}

bool SessionFolder::copy_synced(const std::string& from, const std::string& to, std::string& why) {
    const std::string part = to + ".part";
    std::error_code ec;
    fs::remove(part, ec);
    std::FILE* in = std::fopen(from.c_str(), "rb");
    if (in == nullptr) {
        why = "cannot read " + from + " to copy it";
        return false;
    }
    std::FILE* out = platform::create_exclusive(part);
    if (out == nullptr) {
        std::fclose(in);
        why = "cannot write " + part;
        return false;
    }
    std::vector<char> buffer(std::size_t{1} << 20);
    bool ok = true;
    for (;;) {
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), in);
        if (got > 0 && std::fwrite(buffer.data(), 1, got, out) != got) ok = false;
        if (got < buffer.size() || !ok) break;
    }
    ok = ok && !std::ferror(in) && platform::sync_data(out);
    std::fclose(in);
    ok = std::fclose(out) == 0 && ok;
    if (ok) fs::rename(part, to, ec);
    if (!ok || ec) {
        std::error_code rm;
        fs::remove(part, rm);
        why = "copying " + from + " to " + to + " failed";
        return false;
    }
    return true;
}

bool SessionFolder::save_as(const std::string& session_file, const std::string& bytes,
                            const model::Snapshot& document, const std::string& source_folder,
                            bool media, std::string& why, std::string& caution, SaveAsReport* report) {
    caution.clear();
    SaveAsReport local;
    SaveAsReport& rep = report != nullptr ? *report : local;
    rep = SaveAsReport{};
    const fs::path file(session_file);
    if (!file.is_absolute() || file.extension() != ".sonora" || file.stem().empty()) {
        why = "a session file is an absolute path ending in \".sonora\", not \"" + session_file + "\"";
        return false;
    }
    const fs::path target = plain(file.parent_path());
    if (target.filename() == kBackupsDir) {
        why = "a session is saved into a folder of its own, not into \"" + std::string(kBackupsDir) + "\"";
        return false;
    }
    std::error_code ec;
    const fs::path source = plain(fs::absolute(source_folder, ec));
    if (source_folder.empty() || ec) {
        why = "the session has no folder to save from";
        return false;
    }
    if (within(target, source) || within(source, target)) {
        why = target == source
                  ? target.string() + " is the session's own folder; use Save"
                  : "a session cannot be saved into its own folder or around it (" + target.string() + ")";
        return false;
    }
    const fs::file_status st = fs::symlink_status(target, ec);
    if (fs::exists(st)) {
        if (!fs::is_directory(st)) {
            why = target.string() + " exists and is not a folder; choose another name";
            return false;
        }
        if (fs::equivalent(target, source, ec)) {
            why = target.string() + " is the session's own folder; use Save";
            return false;
        }
        if (!fs::is_empty(target, ec) || ec) {
            why = target.string() + " exists and is not empty; choose another name";
            return false;
        }
    }

    // The highest folder this call makes: what a failure removes, and
    // nothing above it.
    fs::path made;
    for (fs::path p = target; !p.empty() && !fs::exists(fs::symlink_status(p, ec)); p = p.parent_path()) {
        made = p;
        if (p == p.parent_path()) break;
    }
    fs::create_directories(target, ec);
    if (ec) {
        why = "cannot create the session folder " + target.string();
        return false;
    }
    // In a folder that was there, empty: the entries this call may make in
    // it, which are all a failure removes there (#164 review).
    std::set<std::string> put = {file.filename().string(), file.filename().string() + ".tmp",
                                 kSequenceDir, kPluginStateDir, kBackupsDir};
    const auto fail = [&](std::string reason) {
        std::error_code rm;
        if (!made.empty()) {
            // Only links and copies this call made are in it, and remove_all
            // never follows a symlink.
            fs::remove_all(made, rm);
        } else {
            for (const std::string& name : put) {
                const fs::path p = target / name;
                if (fs::exists(fs::symlink_status(p, rm))) fs::remove_all(p, rm);
            }
        }
        folder_.clear();
        swept_.erase(target.string());
        const std::string under = (target / "").string();
        for (auto it = durable_states_.begin(); it != durable_states_.end();)
            it = it->compare(0, under.size(), under) == 0 ? durable_states_.erase(it) : std::next(it);
        why = std::move(reason);
        return false;
    };
    // Each new folder's entry is durable once its parent is synced.
    if (!made.empty()) {
        for (fs::path p = target; within(p, made); p = p.parent_path()) {
            if (!sync_directory_(p.parent_path().string())) {
                return fail("cannot sync " + p.parent_path().string() + " after creating " + p.string());
            }
            if (p == made) break;
        }
    }

    if (media) {
        // Every file of the media folders, not only those the document
        // names: a take not yet placed, or left, is still a recording.
        std::set<std::string> files;
        for (const char* sub : {kAudioDir, kMidiFilesDir}) {
            const fs::path base = source / sub;
            if (!fs::is_directory(fs::symlink_status(base, ec))) continue;
            // A directory symlink is not walked into: its files are not the
            // session's.
            for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code type;
                const fs::file_status s = it->symlink_status(type);
                const std::string name = it->path().filename().string();
                if (type || fs::is_directory(s) || ends_with(name, ".part") || ends_with(name, ".tmp")) continue;
                if (fs::is_regular_file(s) || (fs::is_symlink(s) && fs::is_regular_file(it->status(type)))) {
                    files.insert(it->path().lexically_relative(source).generic_string());
                }
            }
            if (ec) return fail("cannot list " + base.string() + ": " + ec.message());
        }
        std::vector<std::string> named;
        if (document.sources) {
            for (const model::Source& s : document.sources->sources) named.push_back(s.path);
            for (const model::MidiSource& s : document.sources->midi_sources) named.push_back(s.path);
        }
        for (const std::string& path : named) {
            if (files.count(path) != 0) continue;
            std::error_code type;
            if (inside_relative(path) && fs::is_regular_file(fs::status(source / path, type))) {
                files.insert(path);
            } else {
                rep.missing.push_back(path);
            }
        }

        std::set<fs::path> dirs;   // made here, each synced once at the end
        std::vector<std::string> to_copy;
        for (const std::string& rel : files) {
            const fs::path from = source / rel;
            const fs::path to = target / rel;
            put.insert(fs::path(rel).begin()->string());
            for (fs::path d = to.parent_path(); within(d, target) && plain(d) != target; d = d.parent_path()) {
                dirs.insert(plain(d));
            }
            fs::create_directories(to.parent_path(), ec);
            if (ec) return fail("cannot create " + to.parent_path().string());
            std::string link_why;
            // A symlink's target is copied: linking would share the link,
            // which points outside the new folder.
            if (!fs::is_symlink(fs::symlink_status(from, ec)) &&
                platform::hard_link(from.string(), to.string(), link_why)) {
                ++rep.linked;
                continue;
            }
            to_copy.push_back(rel);
        }
        std::uint64_t needed = 0;
        for (const std::string& rel : to_copy) {
            const std::uintmax_t size = fs::file_size(source / rel, ec);
            if (!ec) needed += size;
        }
        std::uint64_t available = 0;
        if (needed > 0 && platform::free_bytes(target.string(), available) && needed > available) {
            return fail("the audio to copy is " + megabytes(needed) + " and " + target.string() +
                        " has " + megabytes(available) + " free");
        }
        for (const std::string& rel : to_copy) {
            std::string copy_why;
            if (!copy_synced((source / rel).string(), (target / rel).string(), copy_why)) {
                return fail(copy_why);
            }
            ++rep.copied;
            rep.copied_bytes += fs::file_size(target / rel, ec);
        }
        for (const fs::path& d : dirs) {
            if (!sync_directory_(d.string())) return fail("cannot sync " + d.string());
        }
        if (!dirs.empty() && !sync_directory_(target.string())) return fail("cannot sync " + target.string());
    }

    std::string save_why, save_caution;
    if (!save(file.string(), bytes, document, save_why, save_caution, source.string())) {
        return fail(save_why);
    }
    caution = save_caution;
    if (!rep.missing.empty()) {
        std::string line = std::to_string(rep.missing.size()) +
                           (rep.missing.size() == 1 ? " file was" : " files were") +
                           " already missing and stay offline: " + rep.missing.front();
        if (rep.missing.size() > 1) line += " and others";
        caution = caution.empty() ? line : caution + "; " + line;
    }
    return true;
}

bool SessionFolder::write_states(const std::string& folder, const std::vector<StateFile>& states,
                                 std::string& why) {
    if (states.empty()) return true;
    sweep_states(folder);
    if (!make_state_dir(folder, why)) return false;
    std::vector<std::string> placed;
    for (const StateFile& s : states) {
        // The name is the hash: bytes that are not the digest's must never
        // carry it, or the check at open refuses a state we wrote.
        if (s.bytes.size() > kMaxStateBytes || sha256_hex(s.bytes) != s.sha256) {
            why = "a plugin state does not match its digest " + s.sha256;
            return false;
        }
        const fs::path file = fs::path(folder) / model::plugin_state_file(s.sha256);
        placed.push_back(file.string());
        std::string there;
        std::error_code ec;
        if (fs::exists(file, ec) && read_file(file, kMaxStateBytes, there) && there == s.bytes) continue;
        if (!write_synced(file, s.bytes, why)) return false;
        ++states_written_;
    }
    return sync_states(folder, placed, why);
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
