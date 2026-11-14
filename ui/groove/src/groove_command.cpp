// SPDX-License-Identifier: GPL-3.0-or-later
//
// The groove words of tempo.edit (ADR-0115). Each word picks a template; the
// edit itself is groove_apply's. Only the public EditController API is used,
// so the module needs no change to the controller.
#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

#include "edit_controller.h"
#include "groove_detail.h"
#include "model_access.h"
#include "sonora/model/musical_map.h"
#include "sonora/ui_groove/onset_source.h"
#include "sonora/ui_groove/template_store.h"

namespace sonora::ui {

using model::Frames;

namespace {

constexpr std::size_t kDefaultReferenceSlots = 4;

const char* const kUsage =
    "groove swing <50-75> <8|16|32> [strength], groove region [slots per beat] [strength], "
    "groove saved <name> [strength], or groove save <name> swing|region ...";

// Words split on spaces; a double-quoted run is one word, so a saved name
// may hold spaces. False on an unclosed quote.
bool split_words(const std::string& text, std::vector<std::string>& out) {
    std::string word;
    bool quoted = false, any = false;
    for (char ch : text) {
        if (ch == '"') {
            quoted = !quoted;
            any = true;
        } else if (ch == ' ' && !quoted) {
            if (any) out.push_back(word);
            word.clear();
            any = false;
        } else {
            word += ch;
            any = true;
        }
    }
    if (any) out.push_back(word);
    return !quoted;
}

std::optional<double> number(std::string s) {
    if (!s.empty() && s.back() == '%') s.pop_back();
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

// An optional trailing strength at words[at]; nothing after it.
bool strength_at(const std::vector<std::string>& w, std::size_t at, double& strength,
                 std::string& why) {
    strength = 100.0;
    if (w.size() <= at) return true;
    const auto v = number(w[at]);
    if (!v || w.size() > at + 1) {
        why = kUsage;
        return false;
    }
    if (!(*v > 0.0 && *v <= 100.0)) {
        why = "groove strength is a percent above 0 and at most 100";
        return false;
    }
    strength = *v;
    return true;
}

// Swing on eighths means pairs of eighths: slots per beat follow the note
// of the meter in force where the selection starts.
bool swing_template(const EditController& ec, const std::string& pct, const std::string& sub,
                    groove::GrooveTemplate& out, std::string& why) {
    const auto p = number(pct), d = number(sub);
    if (!p || !d || *d != std::floor(*d) || *d < 1) {
        why = kUsage;
        return false;
    }
    model::MusicalMap map;
    if (!map.build(ec.snapshot().settings, why)) return false;
    const Selection& sel = ec.selection();
    const auto note = map.entries()[map.entry_at_frame(sel.has_range() ? sel.start : sel.insertion)].note;
    const auto div = static_cast<std::uint32_t>(*d);
    if (div % note != 0 || (div / note) % 2 != 0) {
        why = "swing on 1/" + sub + " notes needs pairs of them in a beat; the meter here counts 1/" +
              std::to_string(note) + " notes, so try " + std::to_string(note * 2) + " or " +
              std::to_string(note * 4);
        return false;
    }
    return groove::groove_swing(*p, div / note, out, why);
}

bool slots_word(const std::vector<std::string>& w, std::size_t at, std::size_t& slots,
                std::string& why) {
    slots = kDefaultReferenceSlots;
    if (w.size() <= at) return true;
    const auto v = number(w[at]);
    if (!v || *v != std::floor(*v) || *v < 1 || *v > groove::kMaxGrooveSlots) {
        why = "slots per beat is a whole number, 1 to 64";
        return false;
    }
    slots = static_cast<std::size_t>(*v);
    return true;
}

bool apply_reported(EditController& ec, const groove::GrooveTemplate& tmpl, double strength,
                    groove_detail::Targets which, std::string& why) {
    GrooveReport report;
    if (!groove_detail::apply(ec, tmpl, strength, which, report, why)) return false;
    if (report.overlapping == 0 && report.grouped == 0) return true;
    // Done, but not all of it. Only a false shows its reason, and a region
    // left behind unannounced looks grooved when it is not.
    why = report.message + ". Undo puts the moved ones back.";
    return false;
}

bool need_onsets(const GrooveDeps& deps, std::string& why) {
    if (deps.onsets != nullptr) return true;
    why = "a groove from a region needs the waveform overview, which tempo.edit cannot reach "
          "yet; use a swing or a saved groove. Nothing was moved.";
    return false;
}

bool need_store(const GrooveDeps& deps, std::string& why) {
    if (deps.store != nullptr) return true;
    why = "there is no preferences directory to keep grooves in (set XDG_CONFIG_HOME or HOME)";
    return false;
}

bool save_word(EditController& ec, const GrooveDeps& deps, const std::vector<std::string>& w,
               std::string& why) {
    // save <name> swing <pct> <sub> | save <name> region [slots]
    if (w.size() < 3) {
        why = kUsage;
        return false;
    }
    if (!need_store(deps, why)) return false;
    groove::GrooveTemplate tmpl;
    if (w[2] == "swing" && w.size() == 5) {
        if (!swing_template(ec, w[3], w[4], tmpl, why)) return false;
    } else if (w[2] == "region" && w.size() <= 4) {
        std::size_t slots = 0;
        if (!need_onsets(deps, why) || !slots_word(w, 3, slots, why) ||
            !groove_from_reference(ec, *deps.onsets, slots, tmpl, why)) {
            return false;
        }
    } else {
        why = kUsage;
        return false;
    }
    return deps.store->save(w[1], tmpl, why);
}

}  // namespace

bool groove_from_reference(const EditController& ec, const OnsetSource& onsets,
                           std::size_t slots_per_beat, groove::GrooveTemplate& out,
                           std::string& why) {
    const Selection& sel = ec.selection();
    const auto& tracks = ec.snapshot().edit->tracks;
    if (!sel.has_range() || sel.track < 0 || sel.track >= static_cast<int>(tracks.size())) {
        why = "select a range from the reference track down over the tracks to groove";
        return false;
    }
    const model::TrackEdit* ref = tracks[static_cast<std::size_t>(sel.track)].get();
    const model::Playlist* p = active_playlist(ref);
    if (p == nullptr) {
        why = ref->name + " has no audio playlist to take a groove from";
        return false;
    }
    const std::uint32_t rate = ec.snapshot().settings.sample_rate;
    std::vector<Frames> hits;
    for (const model::Region& r : p->regions) {
        if (r.end() <= sel.start || r.position >= sel.end) continue;
        std::vector<Frames> found;
        if (!onsets.onsets(r, rate, found, why)) return false;
        for (Frames f : found) {
            if (f >= sel.start && f < sel.end) hits.push_back(f);
        }
    }
    if (hits.empty()) {
        why = "the reference track (" + ref->name + ") has no hits inside the selected range";
        return false;
    }
    model::MusicalMap map;
    if (!map.build(ec.snapshot().settings, why)) return false;
    return groove::groove_from_onsets(map, hits, slots_per_beat, out, why);
}

bool groove_command(EditController& ec, const GrooveDeps& deps, const std::string& text,
                    std::string& why) {
    std::vector<std::string> w;
    if (!split_words(text, w)) {
        why = "a quoted name is not closed";
        return false;
    }
    if (!w.empty() && w[0] == "groove") w.erase(w.begin());
    if (w.empty()) {
        why = kUsage;
        return false;
    }
    const std::string& verb = w[0];
    groove::GrooveTemplate tmpl;
    double strength = 100.0;
    if (verb == "swing") {
        if (w.size() < 3) {
            why = kUsage;
            return false;
        }
        return swing_template(ec, w[1], w[2], tmpl, why) && strength_at(w, 3, strength, why) &&
               apply_reported(ec, tmpl, strength, groove_detail::Targets::Selection, why);
    }
    if (verb == "region") {
        std::size_t slots = 0;
        return need_onsets(deps, why) && slots_word(w, 1, slots, why) &&
               strength_at(w, 2, strength, why) &&
               groove_from_reference(ec, *deps.onsets, slots, tmpl, why) &&
               apply_reported(ec, tmpl, strength, groove_detail::Targets::BelowReference, why);
    }
    if (verb == "saved") {
        if (w.size() < 2) {
            why = kUsage;
            return false;
        }
        return need_store(deps, why) && strength_at(w, 2, strength, why) &&
               deps.store->find(w[1], tmpl, why) &&
               apply_reported(ec, tmpl, strength, groove_detail::Targets::Selection, why);
    }
    if (verb == "save") return save_word(ec, deps, w, why);
    why = kUsage;
    return false;
}

bool groove_command(EditController& ec, const std::string& text, std::string& why) {
    const std::string file = default_groove_templates_file();
    const GrooveTemplateStore store(file);
    GrooveDeps deps;
    deps.store = file.empty() ? nullptr : &store;
    return groove_command(ec, deps, text, why);
}

}  // namespace sonora::ui
