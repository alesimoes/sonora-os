// SPDX-License-Identifier: GPL-3.0-or-later
//
// The elastic words of tempo.edit (ADR-0136). The words are turned into the
// contract's own argument text and read back with parse_elastic_request, so
// the interface refuses exactly what the engine would, in the same words.
#include "sonora/ui_elastic/elastic_command.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "edit_controller.h"
#include "model_access.h"
#include "sonora/model/musical_map.h"

namespace sonora::ui {

namespace {

namespace detail = api::elastic_detail;

const char* const kUsage =
    "elastic ratio <0.25-4> [algorithm], elastic quantize <grid> [strength] [algorithm] or "
    "elastic warp <from:to,...> [algorithm]; the algorithm is polyphonic, rhythmic, "
    "monophonic or varispeed";

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    for (const char ch : text) {
        if (ch == ' ' || ch == '\t') {
            if (!word.empty()) out.push_back(word);
            word.clear();
        } else {
            word += ch;
        }
    }
    if (!word.empty()) out.push_back(word);
    return out;
}

// The optional trailing algorithm at words[at]; nothing after it.
bool algorithm_at(const std::vector<std::string>& w, std::size_t at, std::string& algo,
                  std::string& why) {
    algo = api::elastic_algorithm_name(api::ElasticAlgorithm::Polyphonic);
    if (w.size() <= at) return true;
    if (w.size() > at + 1) {
        why = kUsage;
        return false;
    }
    api::ElasticAlgorithm a{};
    if (!detail::parse_algorithm(w[at], a)) {
        why = "\"" + w[at] + "\" is not an elastic algorithm. " + kUsage;
        return false;
    }
    algo = w[at];
    return true;
}

bool is_algorithm(const std::string& word) {
    api::ElasticAlgorithm a{};
    return detail::parse_algorithm(word, a);
}

// "1/16" is a sixteenth note, in beats of the meter in force at `at` (a beat
// is the meter's denominator note); anything else is beats as written.
bool grid_beats(const EditController& ec, const std::string& word, model::Frames at,
                std::string& beats, std::string& why) {
    const std::size_t slash = word.find('/');
    if (slash == std::string::npos) {
        beats = word;
        return true;
    }
    int64_t num = 0, den = 0;
    if (!detail::parse_whole(word.substr(0, slash), num) ||
        !detail::parse_whole(word.substr(slash + 1), den) || num == 0 || den == 0) {
        why = "the grid \"" + word + "\" is a note (1/16) or a number of beats (0.25)";
        return false;
    }
    model::MusicalMap map;
    if (!map.build(ec.snapshot().settings, why)) return false;
    const std::uint32_t note = map.entries()[map.entry_at_frame(at)].note;
    beats = detail::format_micro(static_cast<double>(num) * note / static_cast<double>(den));
    return true;
}

// The mode word of the contract's argument, from the operator's words.
bool mode_word(const EditController& ec, const model::Region& region,
               const std::vector<std::string>& w, std::string& mode, std::string& algo,
               std::string& why) {
    const std::string& verb = w[0];
    if (verb == "ratio" || verb == "warp") {
        if (w.size() < 2) {
            why = kUsage;
            return false;
        }
        mode = verb + "=" + w[1];
        return algorithm_at(w, 2, algo, why);
    }
    if (verb == "quantize") {
        std::string beats;
        if (w.size() < 2 || !grid_beats(ec, w[1], region.position, beats, why)) {
            if (why.empty()) why = kUsage;
            return false;
        }
        std::size_t next = 2;
        std::string strength = "100";
        if (w.size() > next && !is_algorithm(w[next])) {
            strength = w[next];
            if (!strength.empty() && strength.back() == '%') strength.pop_back();
            ++next;
        }
        mode = "quantize=" + beats + ":" + strength;
        return algorithm_at(w, next, algo, why);
    }
    why = kUsage;
    return false;
}

// The region that starts first at or after `r`'s end on the same playlist.
const model::Region* next_region(const model::TrackEdit& t, const model::Region& r) {
    const model::Playlist* p = active_playlist(&t);
    const model::Region* next = nullptr;
    if (p == nullptr) return nullptr;
    for (const model::Region& g : p->regions) {
        if (g.id == r.id || g.position < r.end()) continue;
        if (next == nullptr || g.position < next->position) next = &g;
    }
    return next;
}

bool fits_before_next(const model::TrackEdit& t, const model::Region& r,
                      const api::ElasticRequest& req, std::string& why) {
    const int64_t out = api::elastic_output_frames(req, r.length);
    const model::Region* next = next_region(t, r);
    if (out < 0 || next == nullptr || r.position + out <= next->position) return true;
    why = "stretched, \"" + r.name + "\" would run " +
          std::to_string(r.position + out - next->position) + " frames into \"" + next->name +
          "\"; regions never overlap, so move or trim \"" + next->name +
          "\" first. Nothing was stretched.";
    return false;
}

}  // namespace

bool elastic_request(const EditController& ec, const std::string& text, api::ElasticRequest& out,
                     std::string& why) {
    std::vector<std::string> w = split_words(text);
    if (!w.empty() && w[0] == "elastic") w.erase(w.begin());
    if (w.empty()) {
        why = kUsage;
        return false;
    }
    const model::Region* r = ec.selected_region(why);
    if (r == nullptr) return false;
    const model::TrackEdit* track = nullptr;
    find_region(ec.snapshot(), r->id, &track);
    if (track == nullptr || r->channels.empty()) {
        why = "\"" + r->name + "\" is not an audio region; only audio stretches";
        return false;
    }
    std::string mode, algo;
    if (!mode_word(ec, *r, w, mode, algo, why)) return false;
    const std::string argument = std::to_string(r->id) + " " + algo + " " + mode;
    api::ElasticRequest req;
    if (!api::parse_elastic_request(argument, req, why)) return false;
    if (!api::validate_elastic_request(req, r->length, why)) return false;
    if (!fits_before_next(*track, *r, req, why)) return false;
    out = std::move(req);
    return true;
}

bool elastic_available(const ElasticDeps& deps, std::string& why) {
    const auto* caps = deps.capabilities;
    if (caps == nullptr ||
        std::find(caps->begin(), caps->end(), api::kCapabilityElastic) == caps->end()) {
        why = "Elastic Audio is not available: this engine was built without the time "
              "stretcher. Nothing was stretched.";
        return false;
    }
    if (!deps.send) {
        why = "Elastic Audio needs the engine link, which is not connected. Nothing was "
              "stretched.";
        return false;
    }
    return true;
}

bool elastic_command(EditController& ec, const ElasticDeps& deps, const std::string& text,
                     std::string& why) {
    if (!elastic_available(deps, why)) return false;
    api::ElasticRequest req;
    if (!elastic_request(ec, text, req, why)) return false;
    deps.send("audio.elastic", api::format_elastic_request(req));
    return true;
}

bool elastic_command(EditController& ec, const std::string& text, std::string& why) {
    api::ElasticRequest req;
    if (!elastic_request(ec, text, req, why)) return false;
    why = "Elastic Audio cannot reach the engine from tempo.edit yet; nothing was stretched. "
          "The engine command is: audio.elastic " +
          api::format_elastic_request(req);
    return false;
}

}  // namespace sonora::ui
