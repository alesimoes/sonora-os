// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/stems.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace sonora {

namespace fs = std::filesystem;

namespace {

// As bounce.cpp's: a track or a folder name never makes a path of its own.
std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

bool audible_audio(const model::Snapshot& s, const model::TrackEdit& t) {
    if (t.kind != model::TrackKind::Audio) return false;
    const model::TrackMix* m = s.mix.find(t.id);
    return m == nullptr || !m->mute;
}

const model::TrackEdit* find_track(const model::Snapshot& s, model::Id id) {
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

std::string output_name(const model::Snapshot& s, model::Id output) {
    if (output == model::kMasterOutput) return "Master";
    for (const model::Bus& b : s.edit->buses) {
        if (b.id == output) return b.name.empty() ? "Bus " + std::to_string(output) : b.name;
    }
    return "Output " + std::to_string(output);
}

bool plan_by_track(const model::Snapshot& s, const api::StemsOptions& o, StemsPlan& out,
                   std::string& error) {
    if (o.tracks.empty()) {
        for (const auto& t : s.edit->tracks) {
            if (audible_audio(s, *t)) out.stems.push_back(Stem{t->name, {t->id}});
        }
        return true;
    }
    for (const model::Id id : o.tracks) {
        const model::TrackEdit* t = find_track(s, id);
        if (t == nullptr || t->kind != model::TrackKind::Audio) {
            error = "track " + std::to_string(id) + " is not an audio track of this session";
            return false;
        }
        // A muted track's stem is a silent file that looks like a mistake.
        if (!audible_audio(s, *t)) {
            error = "track \"" + t->name + "\" is muted; unmute it or leave it out of the stems";
            return false;
        }
        out.stems.push_back(Stem{t->name, {id}});
    }
    return true;
}

void plan_by_output(const model::Snapshot& s, StemsPlan& out) {
    std::vector<model::Id> outputs;
    for (const auto& t : s.edit->tracks) {
        if (!audible_audio(s, *t)) continue;
        const auto at = std::find(outputs.begin(), outputs.end(), t->output);
        if (at == outputs.end()) {
            outputs.push_back(t->output);
            out.stems.push_back(Stem{output_name(s, t->output), {t->id}});
        } else {
            out.stems[static_cast<std::size_t>(at - outputs.begin())].tracks.push_back(t->id);
        }
    }
}

std::string export_name(const model::Snapshot& s, const api::StemsOptions& o) {
    if (!o.base.name.empty()) return file_safe(o.base.name);
    return file_safe(s.settings.name.empty() ? std::string("Untitled") : s.settings.name);
}

uint64_t range_end(const model::Snapshot& s, const api::BounceOptions& o) {
    return o.end != 0 ? o.end : static_cast<uint64_t>(bounce_extent(s));
}

bool any_active_insert(const model::Snapshot& s) {
    for (const auto& t : s.edit->tracks) {
        for (const model::Insert& i : t->inserts) {
            if (i.active) return true;
        }
    }
    return false;
}

std::string extension_of(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return path.substr(dot);
}

}  // namespace

bool stems_plan(const model::Snapshot& snapshot, const api::StemsOptions& opts, StemsPlan& out,
                std::string& error) {
    out = StemsPlan{};
    out.folder = api::stems_folder(export_name(snapshot, opts));
    if (opts.by == api::StemsBy::Output) {
        plan_by_output(snapshot, out);
    } else if (!plan_by_track(snapshot, opts, out, error)) {
        return false;
    }
    if (out.stems.empty()) {
        error = "there are no stems to export: no audio track is unmuted";
        return false;
    }
    return true;
}

bool stems_check(const model::Snapshot& snapshot, const std::string& session_dir,
                 const api::StemsOptions& opts, std::string& error) {
    StemsPlan plan;
    if (!stems_plan(snapshot, opts, plan, error)) return false;
    if (range_end(snapshot, opts.base) <= opts.base.start) {
        error = "there is nothing to export: the range is empty";
        return false;
    }
    // bounce_check's reason would point at the real-time mode, which stems
    // refuse; freezing prints the inserts into the track instead.
    if (any_active_insert(snapshot)) {
        error = "stems render offline, which does not run plug-in inserts; freeze or commit "
                "the tracks with active inserts first";
        return false;
    }
    if (opts.base.normalise) {
        error = "stems are not normalised: a gain of its own per file would stop them summing "
                "to the mix; normalise the mix bounce instead";
        return false;
    }
    api::BounceOptions offline = opts.base;
    offline.mode = api::BounceMode::Offline;
    if (!bounce_check(snapshot, offline, error)) return false;
    std::error_code ec;
    if (fs::exists(session_dir + "/" + plan.folder, ec)) {
        error = "\"" + plan.folder + "\" is already in the session; give the stems another name";
        return false;
    }
    return true;
}

model::Snapshot stem_snapshot(const model::Snapshot& snapshot, const Stem& stem) {
    model::Snapshot s = snapshot;
    const auto in_stem = [&](model::Id id) {
        return std::find(stem.tracks.begin(), stem.tracks.end(), id) != stem.tracks.end();
    };
    for (model::TrackMix& m : s.mix.tracks) {
        m.solo = in_stem(m.track);
        // Solo safe would put the track in every stem, and the sum past the mix.
        m.solo_safe = false;
    }
    for (const model::Id id : stem.tracks) {
        if (s.mix.find(id) != nullptr) continue;
        model::TrackMix m;
        m.track = id;
        m.solo = true;
        s.mix.tracks.push_back(m);
    }
    // A soloed bus solos what feeds it (ADR-0024 §4), which is not the stem.
    for (model::BusMix& b : s.mix.buses) b.solo = false;
    return s;
}

std::string stem_file_stem(std::size_t index, const std::string& name) {
    char digits[24];
    std::snprintf(digits, sizeof digits, "%02zu", index + 1);
    return std::string(digits) + " " + file_safe(name.empty() ? std::string("Track") : name);
}

void remove_stems_folder(const std::string& session_dir, const std::string& folder) {
    if (folder.empty()) return;
    std::error_code ec;
    fs::remove_all(session_dir + "/" + folder, ec);
}

bool stems_offline(const model::Snapshot& snapshot, const std::string& session_dir,
                   const api::StemsOptions& opts, const BounceHooks& hooks, StemsResult& out,
                   std::string& error, const StemRenderer& render) {
    out = StemsResult{};
    if (!stems_check(snapshot, session_dir, opts, error)) return false;
    StemsPlan plan;
    if (!stems_plan(snapshot, opts, plan, error)) return false;

    std::error_code ec;
    fs::create_directories(session_dir + "/" + kBouncedDir, ec);
    // Made here and only here, so removing it on failure removes nothing the
    // operator had: a folder that appeared since the check is refused.
    if (!fs::create_directory(session_dir + "/" + plan.folder, ec) || ec) {
        error = "cannot create \"" + plan.folder + "\"" + (ec ? ": " + ec.message() : "");
        return false;
    }
    const auto fail = [&](const std::string& because) {
        remove_stems_folder(session_dir, plan.folder);
        error = because;
        return false;
    };
    const auto cancelled = [&]() {
        return hooks.cancel != nullptr && hooks.cancel->load(std::memory_order_relaxed);
    };

    // One range for every pass, resolved once, so every file lines up.
    api::BounceOptions pass = opts.base;
    pass.mode = api::BounceMode::Offline;
    pass.end = range_end(snapshot, opts.base);
    const double n = static_cast<double>(plan.stems.size());
    for (std::size_t k = 0; k < plan.stems.size(); ++k) {
        if (cancelled()) return fail("the stem export was cancelled");
        const Stem& stem = plan.stems[k];
        const std::string file = stem_file_stem(k, stem.name);
        // Rendered beside the folder under a name of the bounce's own
        // choosing, then moved in: the bounce picks only names in
        // Bounced Files itself.
        pass.name = plan.folder.substr(std::string(api::kStemsParent).size()) + " " + file;
        BounceHooks h;
        h.cancel = hooks.cancel;
        if (hooks.progress) {
            h.progress = [&hooks, k, n](double f) {
                hooks.progress((static_cast<double>(k) + f) / n);
            };
        }
        BounceResult r;
        std::string why;
        if (!render(stem_snapshot(snapshot, stem), session_dir, pass, h, r, why)) {
            if (cancelled()) return fail("the stem export was cancelled");
            return fail("stem \"" + file + "\": " + why);
        }
        const std::string kept = plan.folder + "/" + file + extension_of(r.relative_path);
        fs::rename(session_dir + "/" + r.relative_path, session_dir + "/" + kept, ec);
        if (ec) {
            std::remove((session_dir + "/" + r.relative_path).c_str());
            return fail("cannot move stem \"" + file + "\" into \"" + plan.folder +
                        "\": " + ec.message());
        }
        out.files.push_back(kept);
        out.frames = r.frames;
    }
    out.folder = plan.folder;
    return true;
}

}  // namespace sonora
