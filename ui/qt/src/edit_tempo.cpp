// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tempo editing (time.tempo_edit, ADR-0050): the model's tempo operations
// sent as a SetSettings with the map replaced, and a stack of their own to
// undo them. The map is session settings, outside the edit undo like the
// loop and punch ranges, so Undo Tempo is separate from Undo.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller.h"
#include "sonora/model/tempo_edit.h"
// ui/groove (ADR-0115) is configured only in the Linux build; the portable
// one compiles this file without it, and there the word reads as a typo.
#if __has_include("sonora/ui_groove/groove_command.h")
#include "sonora/ui_groove/groove_command.h"
#define SONORA_UI_GROOVE 1
#endif
#if __has_include("sonora/ui_beat/beat_command.h")
#include "sonora/ui_beat/beat_command.h"
#define SONORA_UI_BEAT 1
#endif
#if __has_include("sonora/ui_elastic/elastic_command.h")
#include "sonora/ui_elastic/elastic_command.h"
#define SONORA_UI_ELASTIC 1
#endif

namespace sonora::ui {

using namespace model;

namespace {
// How many tempo edits Undo Tempo can step back over. A step is dropped, not
// applied, once the map is no longer the one it made: another interface or
// a settings change got there first.
constexpr std::size_t kTempoSteps = 64;
}  // namespace

bool EditController::submit_tempo(std::vector<TempoEntry> next, std::string& why) {
    SetSettings op{snapshot().settings};
    const std::vector<TempoEntry> before = op.settings.tempo;
    if (next == before) return true;
    op.settings.tempo = next;
    if (!submit(Operation{std::move(op)}, why)) return false;
    tempo_redo_.clear();
    tempo_undo_.push_back({before, std::move(next)});
    if (tempo_undo_.size() > kTempoSteps) tempo_undo_.erase(tempo_undo_.begin());
    return true;
}

bool EditController::set_tempo(Frames at, float bpm, std::string& why) {
    std::vector<TempoEntry> map = snapshot().settings.tempo;
    if (!tempo_set_at(map, snapshot().settings.sample_rate, at, bpm, why)) return false;
    return submit_tempo(std::move(map), why);
}

bool EditController::ramp_tempo(Frames from, Frames to, float bpm_from, float bpm_to,
                                std::string& why) {
    std::vector<TempoEntry> map = snapshot().settings.tempo;
    if (map.empty()) {
        why = "the session has no tempo map";
        return false;
    }
    if (!(bpm_from > 0.0f)) bpm_from = map[tempo_entry_in_force(map, from)].bpm;
    if (!tempo_ramp(map, snapshot().settings.sample_rate, from, to, bpm_from, bpm_to, why)) {
        return false;
    }
    return submit_tempo(std::move(map), why);
}

bool EditController::scale_tempo(Frames from, Frames to, double factor, std::string& why) {
    std::vector<TempoEntry> map = snapshot().settings.tempo;
    if (!tempo_scale(map, snapshot().settings.sample_rate, from, to, factor, why)) return false;
    return submit_tempo(std::move(map), why);
}

bool EditController::remove_tempo(Frames at, std::string& why) {
    std::vector<TempoEntry> map = snapshot().settings.tempo;
    if (map.size() < 2) {
        why = "the session has one tempo and no change to remove";
        return false;
    }
    std::size_t nearest = 1;
    for (std::size_t i = 2; i < map.size(); ++i) {
        if (std::llabs(map[i].at - at) < std::llabs(map[nearest].at - at)) nearest = i;
    }
    if (!tempo_remove_at(map, snapshot().settings.sample_rate, map[nearest].at, why)) return false;
    return submit_tempo(std::move(map), why);
}

bool EditController::set_meter(Frames near, std::uint32_t beats, std::uint32_t note,
                               std::string& why) {
    std::vector<TempoEntry> map = snapshot().settings.tempo;
    Frames at = 0;
    if (!meter_set_near(map, snapshot().settings.sample_rate, near, beats, note, at, why)) {
        return false;
    }
    if (!submit_tempo(std::move(map), why)) return false;
    set_insertion(at);
    return true;
}

bool EditController::tempo_step(bool undo, std::string& why) {
    auto& from = undo ? tempo_undo_ : tempo_redo_;
    auto& to = undo ? tempo_redo_ : tempo_undo_;
    if (from.empty()) {
        why = undo ? "nothing to undo in the tempo map" : "nothing to redo in the tempo map";
        return false;
    }
    const TempoStep step = from.back();
    if (snapshot().settings.tempo != (undo ? step.after : step.before)) {
        tempo_undo_.clear();
        tempo_redo_.clear();
        why = "the tempo map was changed elsewhere; its history is cleared";
        return false;
    }
    SetSettings op{snapshot().settings};
    op.settings.tempo = undo ? step.before : step.after;
    if (!submit(Operation{std::move(op)}, why)) return false;
    from.pop_back();
    to.push_back(step);
    return true;
}

namespace {

// A whole number or decimal and nothing else, a trailing "bpm" allowed.
bool number_of(std::string s, double& out) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() >= 3 && s.compare(s.size() - 3, 3, "bpm") == 0) s.resize(s.size() - 3);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

std::string strip(const std::string& s) {
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(' ') - b + 1);
}

}  // namespace

bool EditController::edit_tempo(const std::string& raw, std::string& why) {
    const std::string text = strip(raw);
    const Frames where = sel_.has_range() ? sel_.start : sel_.insertion;
    const char* usage =
        "type a tempo (96), a ramp over the selection (120>90), a factor over it (x2), "
        "a meter (3/4), detect (from the clip's beats) or remove";
    double a = 0.0, b = 0.0;
    if (text == "remove") return remove_tempo(sel_.insertion, why);
#ifdef SONORA_UI_GROOVE
    if (text.rfind("groove", 0) == 0) return groove_command(*this, text, why);
#endif
#ifdef SONORA_UI_BEAT
    if (text == "detect" || text.rfind("detect ", 0) == 0) return beat_command(*this, text, why);
#endif
#ifdef SONORA_UI_ELASTIC
    if (text.rfind("elastic", 0) == 0) return elastic_command(*this, text, why);
#endif
    if (number_of(text, a)) return set_tempo(where, static_cast<float>(a), why);
    const std::size_t gt = text.find('>');
    if (gt != std::string::npos) {
        const std::string from = strip(text.substr(0, gt));
        if (!(from.empty() || number_of(from, a)) || !number_of(strip(text.substr(gt + 1)), b)) {
            why = usage;
            return false;
        }
        if (!sel_.has_range()) {
            why = "select the stretch the ramp runs over first";
            return false;
        }
        return ramp_tempo(sel_.start, sel_.end, from.empty() ? 0.0f : static_cast<float>(a),
                          static_cast<float>(b), why);
    }
    if (!text.empty() && (text[0] == 'x' || text[0] == '*') && number_of(text.substr(1), a)) {
        if (!sel_.has_range()) {
            why = "select the stretch to change first";
            return false;
        }
        return scale_tempo(sel_.start, sel_.end, a, why);
    }
    const std::size_t slash = text.find('/');
    if (slash != std::string::npos && number_of(text.substr(0, slash), a) &&
        number_of(text.substr(slash + 1), b) && a >= 1 && b >= 1 && a == std::floor(a) &&
        b == std::floor(b)) {
        return set_meter(where, static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), why);
    }
    why = usage;
    return false;
}

bool EditController::tempo_undo(std::string& why) { return tempo_step(true, why); }
bool EditController::tempo_redo(std::string& why) { return tempo_step(false, why); }

}  // namespace sonora::ui
