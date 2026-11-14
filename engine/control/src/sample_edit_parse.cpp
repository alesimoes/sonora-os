// SPDX-License-Identifier: GPL-3.0-or-later
// The text form of sample.edit: "VERB REGION FROM TO CHANNEL [PARAM]".
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "sample_edit_detail.h"
#include "sonora/model/operations.h"

namespace sonora {

namespace {

struct VerbName {
    const char* name;
    SampleVerb verb;
    const char* label;
};
constexpr VerbName kVerbs[] = {
    {"repair", SampleVerb::Repair, "Repair"},       {"draw", SampleVerb::Draw, "Draw"},
    {"silence", SampleVerb::Silence, "Silence"},    {"reverse", SampleVerb::Reverse, "Reverse"},
    {"invert", SampleVerb::Invert, "Invert"},       {"gain", SampleVerb::Gain, "Gain"},
    {"normalize", SampleVerb::Normalize, "Normalize"},
    {"fade_in", SampleVerb::FadeIn, "Fade In"},     {"fade_out", SampleVerb::FadeOut, "Fade Out"},
    {api::kPitchVerb, SampleVerb::Pitch, "Pitch"},
};

bool whole_number(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno != 0) return false;
    out = v;
    return true;
}

bool real_number(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = static_cast<float>(v);
    return true;
}

}  // namespace

const char* sample_edit_detail::label_of(SampleVerb v) {
    for (const VerbName& n : kVerbs) {
        if (n.verb == v) return n.label;
    }
    return "Edit";
}

bool parse_sample_edit(const std::string& text, SampleEditRequest& out, std::string& why) {
    out = SampleEditRequest{};
    std::istringstream in(text);
    std::string verb, region, from, to, channel, param;
    in >> verb >> region >> from >> to >> channel >> param;
    std::string extra;
    if (in >> extra) {
        why = "sample.edit takes VERB REGION FROM TO CHANNEL [PARAM]; \"" + extra + "\" is left over";
        return false;
    }
    if (channel.empty()) {
        why = "sample.edit takes VERB REGION FROM TO CHANNEL [PARAM]";
        return false;
    }
    bool known = false;
    for (const VerbName& n : kVerbs) {
        if (verb == n.name) {
            out.verb = n.verb;
            known = true;
        }
    }
    if (!known) {
        why = "\"" + verb + "\" is not a sample edit; use repair, draw, silence, reverse, invert, "
              "gain, normalize, fade_in, fade_out or pitch";
        return false;
    }
    long long r = 0, f = 0, t = 0, c = 0;
    if (!whole_number(region, r) || r <= 0 || !whole_number(from, f) || f < 0 ||
        !whole_number(to, t) || t <= f) {
        why = "the region and the range must be whole numbers, the range from 0 and not empty";
        return false;
    }
    out.region = static_cast<model::Id>(r);
    out.from = f;
    out.to = t;
    if (channel == "all") {
        out.channel = -1;
    } else if (whole_number(channel, c) && c >= 1 && c <= 1024) {
        out.channel = static_cast<int>(c - 1);
    } else {
        why = "\"" + channel + "\" is not a channel; give a number from 1, or all";
        return false;
    }
    switch (out.verb) {
        case SampleVerb::Gain:
        case SampleVerb::Normalize: {
            if (param.empty()) {
                if (out.verb == SampleVerb::Gain) {
                    why = "gain needs the change in dB";
                    return false;
                }
                out.db = 0.0f;
            } else if (!real_number(param, out.db)) {
                why = "\"" + param + "\" is not a level in dB";
                return false;
            }
            if (out.db < model::kMinGainDb || out.db > model::kMaxGainDb) {
                why = "the level must be from -144 to 24 dB";
                return false;
            }
            break;
        }
        case SampleVerb::Draw: {
            if (param.empty()) {
                why = "draw needs points as FRAME:LEVEL,FRAME:LEVEL";
                return false;
            }
            std::istringstream pts(param);
            std::string item;
            while (std::getline(pts, item, ',')) {
                const std::size_t colon = item.find(':');
                long long frame = 0;
                float level = 0.0f;
                if (colon == std::string::npos || !whole_number(item.substr(0, colon), frame) ||
                    frame < 0 || !real_number(item.substr(colon + 1), level) || level < -1.0f ||
                    level > 1.0f) {
                    why = "\"" + item + "\" is not a point; give FRAME:LEVEL with the level from -1 to 1";
                    return false;
                }
                if (!out.points.empty() && frame <= out.points.back().first) {
                    why = "the points of a drawing must be in increasing order of frame";
                    return false;
                }
                out.points.emplace_back(frame, level);
                if (out.points.size() > 4096) {
                    why = "a drawing holds at most 4096 points";
                    return false;
                }
            }
            break;
        }
        case SampleVerb::Pitch: return sample_edit_detail::parse_pitch_args(param, out, why);
        default:
            if (!param.empty()) {
                why = std::string(sample_edit_detail::label_of(out.verb)) +
                      " takes no parameter; \"" + param + "\" is left over";
                return false;
            }
    }
    return true;
}

}  // namespace sonora
