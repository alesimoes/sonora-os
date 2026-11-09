// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-session paste's planner (ADR-0142). The naming, umid and id rules are
// Import Session Data's (ADR-0123), and its helpers are shared so the two
// cannot drift apart.
#include "sonora/session_files/paste_sources.h"

#include <set>
#include <utility>

#include "session_import_detail.h"
#include "sonora/session_files/session_folder.h"

namespace sonora::session_files {

namespace {

using import_detail::basename_of;
using import_detail::fold;
using import_detail::with_suffix;

const char* const kNothing = "; nothing was pasted";

// Every name the destination's Audio Files/ may already hold, folded: its
// pool, and the takes the operator chose to leave, which are still files.
std::set<std::string> taken_paths(const model::Snapshot& dest) {
    std::set<std::string> out;
    for (const model::Source& s : dest.sources->sources) out.insert(fold(s.path));
    for (const model::MidiSource& s : dest.sources->midi_sources) out.insert(fold(s.path));
    for (const std::string& t : dest.settings.left_takes) {
        out.insert(fold(t));
        out.insert(fold(std::string(kAudioDir) + "/" + basename_of(t)));
    }
    return out;
}

std::string unique_audio_path(const std::string& source_path, std::set<std::string>& taken) {
    const std::string base = std::string(kAudioDir) + "/" + basename_of(source_path);
    std::string out = base;
    for (int n = 2; taken.count(fold(out)) != 0; ++n) out = with_suffix(base, n);
    taken.insert(fold(out));
    return out;
}

// Same audio by its BWF umid, as Import Session Data reuses it: a source
// that holds at least the clipboard's span on as many channels.
const model::Source* same_audio(const std::vector<model::Source>& pool, const model::Source& s) {
    if (s.umid.empty()) return nullptr;
    for (const model::Source& d : pool) {
        if (d.umid == s.umid && d.channels == s.channels && d.length >= s.length) return &d;
    }
    return nullptr;
}

bool refuse(PasteSourcesPlan& plan, std::string why) {
    plan = PasteSourcesPlan{};
    plan.problem = std::move(why) + kNothing;
    return false;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

bool plan_paste_sources(const model::Snapshot& dest, const PasteSourcesRequest& request,
                        PasteSourcesPlan& plan) {
    plan = PasteSourcesPlan{};
    plan.next_id = dest.next_id;
    std::set<std::string> taken = taken_paths(dest);
    // The sources this plan adds, so two clipboard sources with one umid
    // bring one file.
    std::vector<model::Source> added;
    std::vector<std::string> missing;

    for (const model::Source& s : request.sources) {
        if (plan.remap.count(s.id) != 0) continue;
        const model::Source* here = dest.sources->find(s.id);
        if (here != nullptr && *here == s) {
            plan.remap[s.id] = s.id;
            continue;
        }
        const model::Source* same = same_audio(dest.sources->sources, s);
        if (same == nullptr) same = same_audio(added, s);
        if (same != nullptr) {
            plan.remap[s.id] = same->id;
            continue;
        }
        if (request.origin_folder.empty()) {
            return refuse(plan,
                          "the clipboard's audio was copied from another session and that "
                          "session's folder is not known");
        }
        if (request.origin_sample_rate != 0 &&
            request.origin_sample_rate != dest.settings.sample_rate) {
            return refuse(plan, "the clipboard's audio is at " +
                                    std::to_string(request.origin_sample_rate) +
                                    " Hz and this session at " +
                                    std::to_string(dest.settings.sample_rate) + " Hz");
        }
        FileCopy f;
        f.source_path = request.origin_folder + "/" + s.path;
        if (request.source_readable && !request.source_readable(f.source_path)) {
            missing.push_back(s.path);
            continue;
        }
        model::Source copy = s;
        copy.id = plan.next_id++;
        copy.path = unique_audio_path(s.path, taken);
        // The take belonged to the origin's numbering and track; here it is
        // audio brought in.
        copy.kind = model::SourceKind::Imported;
        copy.take = 0;
        copy.recorded_on_track = 0;
        copy.latency_compensation = 0;
        f.dest_path = copy.path;
        f.source = copy.id;
        plan.copies.push_back(std::move(f));
        plan.remap[s.id] = copy.id;
        added.push_back(copy);
        plan.ops.push_back(model::AddSource{std::move(copy)});
    }

    if (!missing.empty()) {
        std::string why = "the clipboard's audio file " + quoted(missing.front()) +
                          " is no longer in " + quoted(request.origin_folder);
        if (missing.size() > 1) {
            why += " (and " + std::to_string(missing.size() - 1) + " more)";
        }
        return refuse(plan, why);
    }
    return true;
}

bool bring_in_sources(const PasteSourcesPlan& plan, const PasteCopyFn& copy,
                      std::vector<model::Member>& out, std::string& error) {
    error.clear();
    if (!plan.problem.empty()) {
        error = plan.problem;
        return false;
    }
    if (!plan.copies.empty() && !copy(plan.copies, error)) {
        if (error.empty()) error = std::string("the clipboard's audio could not be copied") + kNothing;
        return false;
    }
    out.insert(out.end(), plan.ops.begin(), plan.ops.end());
    return true;
}

PasteCopyFn disk_copier(const std::string& dest_folder, const std::atomic<bool>& cancel,
                        const CopyOptions& options) {
    return [dest_folder, &cancel, options](const std::vector<FileCopy>& copies,
                                           std::string& error) {
        ImportPlan as_import;
        as_import.copies = copies;
        CopyReport report;
        const CopyOutcome outcome =
            perform_copies(as_import, dest_folder, cancel, CopyProgressFn{}, report, options);
        if (outcome == CopyOutcome::Done) return true;
        if (outcome == CopyOutcome::Cancelled) {
            error = "Paste cancelled; nothing was pasted";
            return false;
        }
        // The copier speaks for Import Session Data; the operator pasted.
        static const std::string kImport = "Import Session Data";
        std::string message = report.message;
        if (message.compare(0, kImport.size(), kImport) == 0) {
            message = "Paste" + message.substr(kImport.size());
        }
        error = message + kNothing;
        return false;
    };
}

}  // namespace sonora::session_files
