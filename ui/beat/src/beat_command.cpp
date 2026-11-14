// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_beat/beat_command.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "edit_controller.h"
#include "sonora/analyze/beat_tempo.h"
#include "sonora/platform/process.h"

namespace sonora::ui {

bool ProcessBeatRunner::run(model::Id source, analyze::BeatResult& out, std::string& error) {
    const std::string file = file_of_ ? file_of_(source) : std::string();
    if (file.empty()) {
        error = "the clip's audio file is not known here";
        return false;
    }
    std::string spawn_error;
    auto child = platform::ChildProcess::spawn({exe_, "beat", file, "--out", report_}, {}, spawn_error);
    if (!child) {
        error = "could not start the analyzer: " + spawn_error;
        return false;
    }
    child->wait(-1);
    if (!child->exited_normally() || child->exit_code() != 0) {
        error = "the beat analysis failed";
        return false;
    }
    std::ifstream in(report_, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return analyze::parse_beat_report(ss.str(), out, error);
}

namespace {
const char* const kUsage = "detect [clip|selection] [follow]: set the tempo from the selected clip's beats";

bool parse_words(const std::string& text, bool& need_range, bool& follow, std::string& why) {
    std::istringstream in(text);
    std::string w;
    in >> w;  // "detect"
    need_range = follow = false;
    while (in >> w) {
        if (w == "clip") need_range = false;
        else if (w == "selection") need_range = true;
        else if (w == "follow") follow = true;
        else {
            why = kUsage;
            return false;
        }
    }
    return true;
}
}  // namespace

bool beat_command(EditController& ec, BeatRunner* runner, const std::string& text, std::string& why) {
    bool need_range = false, follow = false;
    if (!parse_words(text, need_range, follow, why)) return false;
    if (!runner) {
        why = "beat detection is not reachable from here";
        return false;
    }
    if (need_range && !ec.selection().has_range()) {
        why = "select a range first, or use: detect clip";
        return false;
    }
    const model::Region* r = ec.selected_region(why);
    if (!r) return false;
    if (r->channels.empty()) {
        why = "the clip has no audio";
        return false;
    }
    analyze::BeatResult res;
    std::string error;
    if (!runner->run(r->channels[0].source, res, error)) {
        why = "no tempo found: " + error;
        return false;
    }
    if (res.confidence < analyze::kBeatMinConfidence) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "the beats are too uncertain (confidence %.2f, need %.2f): the tempo is unchanged",
                      res.confidence, analyze::kBeatMinConfidence);
        why = buf;
        return false;
    }
    if (follow && res.stable) {
        why = "the tempo holds steady: use detect without follow";
        return false;
    }
    const auto& map = ec.snapshot().settings.tempo;
    const std::array<std::uint32_t, 2> meter = map.empty() ? std::array<std::uint32_t, 2>{{4, 4}} : map[0].meter;
    std::vector<model::TempoEntry> next;
    const auto mode = follow ? analyze::TempoFromBeatsMode::follow : analyze::TempoFromBeatsMode::constant;
    if (!analyze::tempo_from_beats(res.beats, res.sample_rate, res.bpm, r->position - r->source_offset, meter,
                                   mode, next, why))
        return false;
    return ec.submit_tempo(std::move(next), why);
}

bool beat_command(EditController& ec, const std::string& text, std::string& why) {
    return beat_command(ec, nullptr, text, why);
}

}  // namespace sonora::ui
