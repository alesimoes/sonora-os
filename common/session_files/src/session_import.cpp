// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data's planner (ADR-0123 decisions 1, 2 and 4): the checks
// that refuse an import before anything is written, and the panel's rows.
// The Compound itself is built by import_detail::Builder.
#include "sonora/session_files/session_import.h"

#include <algorithm>
#include <set>
#include <utility>

#include "session_import_detail.h"
#include "sonora/model/session_file.h"

namespace sonora::session_files {

namespace {

using Reason = ImportRefusal::Reason;

bool refuse(ImportRefusal& refusal, Reason reason, std::string message,
            std::vector<std::string> files = {}) {
    refusal.reason = reason;
    refusal.message = std::move(message);
    refusal.files = std::move(files);
    return false;
}

// Absolute in either form a session folder takes: "/..." on the appliance,
// "C:/..." or "C:\..." in the portable build.
bool absolute(const std::string& path) {
    if (!path.empty() && (path.front() == '/' || path.front() == '\\')) return true;
    return path.size() > 2 && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

const model::TrackEdit* find_track(const model::Snapshot& s, model::Id id) {
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

// What the request asks, against what a snapshot can say about it.
bool check_request(const model::Snapshot& dest, const model::Snapshot& src,
                   const SessionImportRequest& request,
                   std::vector<const model::TrackEdit*>& tracks, ImportRefusal& refusal) {
    if (request.tracks.empty()) {
        return refuse(refusal, Reason::InvalidRequest, "Import Session Data: no track is chosen");
    }
    if (!absolute(request.source_folder)) {
        return refuse(refusal, Reason::InvalidRequest,
                      "Import Session Data: the source session's folder is not known");
    }
    if (request.offset < 0) {
        return refuse(refusal, Reason::InvalidRequest,
                      "Import Session Data: the offset must not be negative");
    }
    std::set<model::Id> seen;
    for (const model::Id id : request.tracks) {
        const model::TrackEdit* t = find_track(src, id);
        if (t == nullptr || !seen.insert(id).second) {
            return refuse(refusal, Reason::InvalidRequest,
                          "Import Session Data: track " + std::to_string(id) +
                              (t == nullptr ? " is not in the source session"
                                            : " is chosen twice"));
        }
        tracks.push_back(t);
    }
    if (request.take_open) {
        return refuse(refusal, Reason::TakeOpen,
                      "Import Session Data: a take is being recorded; stop it first");
    }
    if (request.source_format_version < 1) {
        return refuse(refusal, Reason::InvalidRequest,
                      "Import Session Data: the source session's format version is not known");
    }
    if (request.source_format_version > model::kFormatVersion) {
        return refuse(refusal, Reason::NewerFormat,
                      "Import Session Data: the source session was written by a newer Sonora"
                      " (format " + std::to_string(request.source_format_version) +
                          ", this one reads up to " + std::to_string(model::kFormatVersion) + ")");
    }
    if (src.settings.sample_rate != dest.settings.sample_rate) {
        return refuse(refusal, Reason::SampleRate,
                      "Import Session Data: the source session runs at " +
                          std::to_string(src.settings.sample_rate) + " Hz and this one at " +
                          std::to_string(dest.settings.sample_rate) +
                          " Hz; this version does not resample");
    }
    std::string why;
    if (!model::check_invariants(src, why)) {
        return refuse(refusal, Reason::InvalidRequest,
                      "Import Session Data: the source session is not valid: " + why);
    }
    return true;
}

// The files the plan would copy that the probe cannot read, source-relative.
std::vector<std::string> missing_files(const std::vector<FileCopy>& copies,
                                       const SessionImportRequest& request) {
    std::vector<std::string> missing;
    if (!request.source_readable) return missing;
    const std::size_t prefix = request.source_folder.size() + 1;
    for (const FileCopy& c : copies) {
        if (!request.source_readable(c.source_path)) missing.push_back(c.source_path.substr(prefix));
    }
    return missing;
}

// How many of the tracks, in request order, one Compound still holds.
std::size_t tracks_that_fit(const std::vector<std::size_t>& introduced,
                            const std::vector<std::size_t>& members, std::size_t markers) {
    std::size_t total = markers;
    std::size_t fit = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        total += introduced[i] + members[i];
        if (total > model::kMaxCompoundMembers) break;
        ++fit;
    }
    return fit;
}

}  // namespace

bool plan_session_import(const model::Snapshot& dest, const model::Snapshot& source,
                         const SessionImportRequest& request, ImportPlan& plan,
                         ImportRefusal& refusal) {
    plan = ImportPlan{};
    refusal = ImportRefusal{};
    std::vector<const model::TrackEdit*> tracks;
    if (!check_request(dest, source, request, tracks, refusal)) return false;

    import_detail::Builder build(dest, source, request);
    std::vector<std::size_t> introduced;
    build.add_sources(tracks, introduced);
    std::vector<std::size_t> members;
    for (const model::TrackEdit* t : tracks) members.push_back(build.add_track(*t));
    const std::size_t markers = build.add_markers();
    if (!build.invalid.empty()) {
        return refuse(refusal, Reason::InvalidRequest, "Import Session Data: " + build.invalid);
    }
    if (build.ops.empty()) {
        std::string why = "Import Session Data: nothing to import";
        if (!build.dropped.empty()) why += " (" + build.dropped.front() + ")";
        return refuse(refusal, Reason::InvalidRequest, why);
    }
    std::vector<std::string> missing = missing_files(build.copies, request);
    if (!missing.empty()) {
        // Built first: the order a call's arguments are evaluated in is
        // unspecified, and the move would empty `missing` before it is read.
        std::string message = "Import Session Data: " + std::to_string(missing.size()) +
                              " audio file(s) of the source session cannot be read, first " +
                              missing.front();
        return refuse(refusal, Reason::MissingFile, std::move(message), std::move(missing));
    }
    if (build.ops.size() > model::kMaxCompoundMembers) {
        const std::size_t fit = tracks_that_fit(introduced, members, markers);
        return refuse(refusal, Reason::TooLarge,
                      "Import Session Data: these " + std::to_string(tracks.size()) +
                          " tracks need " + std::to_string(build.ops.size()) +
                          " operations and one import holds " +
                          std::to_string(model::kMaxCompoundMembers) + "; the first " +
                          std::to_string(fit) + " fit, import the rest after them");
    }
    plan.ops.label = "Import Session Data";
    plan.ops.ops = std::move(build.ops);
    plan.copies = std::move(build.copies);
    plan.dropped = std::move(build.dropped);
    plan.approximated = std::move(build.approximated);
    return true;
}

std::vector<ImportableTrack> list_importable_tracks(const model::Snapshot& source) {
    std::vector<ImportableTrack> rows;
    for (const auto& t : source.edit->tracks) {
        ImportableTrack row;
        row.id = t->id;
        row.name = t->name;
        row.kind = t->kind;
        row.channels = t->channels;
        if (t->kind == model::TrackKind::Audio) {
            if (const model::Playlist* p = import_detail::active_playlist(*t)) {
                row.regions = p->regions.size();
                for (const model::Region& r : p->regions) row.length = std::max(row.length, r.end());
            }
        } else {
            for (const auto& p : t->midi_playlists) {
                if (p->id == t->active_playlist) row.regions = p->regions.size();
            }
            row.importable = false;
            row.why = "MIDI and instrument tracks are not imported in this version";
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace sonora::session_files
