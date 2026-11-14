// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loads data/mastering_rules.json into a MasteringRules (ADR-0172). Every
// field is required and typed: a missing key, a wrong type or an out-of-range
// band index is an error naming the key, so a stale or hand-edited file
// fails at start-up and never changes the suggestions silently.
#include <fstream>
#include <sstream>

#include "sonora/analyze/mastering_rules.h"
#include "sonora/json/json.h"

namespace sonora::analyze {

namespace {

using json::Value;

struct Reader {
    std::string error;

    const Value* member(const Value& o, const char* key, const std::string& where) {
        const Value* v = o.is_object() ? o.find(key) : nullptr;
        if (v == nullptr && error.empty()) error = "mastering rules: missing \"" + where + key + "\"";
        return v;
    }
    bool number(const Value& o, const char* key, const std::string& where, double& out) {
        const Value* v = member(o, key, where);
        if (v == nullptr) return false;
        if (!v->is_number() || !v->to_double(out)) {
            if (error.empty()) error = "mastering rules: \"" + where + key + "\" is not a number";
            return false;
        }
        return true;
    }
    bool text(const Value& o, const char* key, std::string& out) {
        const Value* v = member(o, key, "");
        if (v == nullptr) return false;
        if (!v->is_string()) {
            if (error.empty()) error = std::string("mastering rules: \"") + key + "\" is not a string";
            return false;
        }
        out = v->text();
        return true;
    }
    bool integer(const Value& v, int lo, int hi, int& out, const char* what) {
        std::int64_t n = 0;
        if (!v.is_number() || !v.to_int64(n) || n < lo || n > hi) {
            if (error.empty()) error = std::string("mastering rules: ") + what + " must be an integer " + std::to_string(lo) + ".." + std::to_string(hi);
            return false;
        }
        out = static_cast<int>(n);
        return true;
    }
};

bool param_index(const std::string& id, std::uint32_t& out) {
    const native::Descriptor& d = native::mastering_descriptor();
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        if (id == d.params[i].id) {
            out = i;
            return true;
        }
    }
    return false;
}

bool read_curve(Reader& r, const Value& root, MasteringRules& out) {
    const Value* t = r.member(root, "target_db", "");
    const Value* n = r.member(root, "band_names", "");
    if (t == nullptr || n == nullptr) return false;
    if (!t->is_array() || t->items().size() != static_cast<std::size_t>(kBands) || !n->is_array() ||
        n->items().size() != static_cast<std::size_t>(kBands)) {
        r.error = "mastering rules: target_db and band_names need exactly 10 entries";
        return false;
    }
    for (int b = 0; b < kBands; ++b) {
        if (!t->items()[b].is_number() || !t->items()[b].to_double(out.target_db[b]) || !n->items()[b].is_string()) {
            r.error = "mastering rules: target_db entries are numbers and band_names entries strings";
            return false;
        }
        out.band_names[b] = n->items()[b].text();
    }
    const Value* a = r.member(root, "anchor_bands", "");
    if (a == nullptr) return false;
    if (!a->is_array() || a->items().empty()) {
        r.error = "mastering rules: anchor_bands must be a non-empty array";
        return false;
    }
    for (const Value& v : a->items()) {
        int b = 0;
        if (!r.integer(v, 0, kBands - 1, b, "an anchor band")) return false;
        out.anchor_bands.push_back(b);
    }
    return r.number(root, "sub_bass_margin_db", "", out.sub_bass_margin_db);
}

bool read_group(Reader& r, const Value& g, BalanceGroup& out) {
    const Value* bands = r.member(g, "bands", "balance_groups[].");
    if (bands == nullptr) return false;
    if (!bands->is_array() || bands->items().empty() || bands->items().size() > 2) {
        r.error = "mastering rules: a balance group's bands are one or two band indices";
        return false;
    }
    for (std::size_t i = 0; i < bands->items().size(); ++i) {
        if (!r.integer(bands->items()[i], 0, kBands - 1, out.bands[i], "a balance band")) return false;
    }
    std::string gain, freq;
    if (!r.text(g, "rule", out.rule) || !r.text(g, "what", out.what) || !r.text(g, "title", out.title) ||
        !r.text(g, "at", out.at) || !r.text(g, "gain", gain) || !r.number(g, "freq_hz", "balance_groups[].", out.freq_hz)) {
        return false;
    }
    if (!param_index(gain, out.gain)) {
        r.error = "mastering rules: unknown console parameter \"" + gain + "\"";
        return false;
    }
    const Value* f = r.member(g, "freq", "balance_groups[].");
    if (f == nullptr) return false;
    if (f->is_null()) return true;
    if (!f->is_string() || !param_index(f->text(), out.freq)) {
        r.error = "mastering rules: a balance group's freq is null or a console parameter id";
        return false;
    }
    return true;
}

bool read_groups(Reader& r, const Value& root, MasteringRules& out) {
    const Value* gs = r.member(root, "balance_groups", "");
    if (gs == nullptr) return false;
    if (!gs->is_array() || gs->items().empty()) {
        r.error = "mastering rules: balance_groups must be a non-empty array";
        return false;
    }
    for (const Value& g : gs->items()) {
        BalanceGroup bg;
        if (!read_group(r, g, bg)) return false;
        out.groups.push_back(std::move(bg));
    }
    return true;
}

bool read_thresholds(Reader& r, const Value& root, MasteringRules& o) {
    const Value* d = r.member(root, "dynamics", "");
    const Value* s = r.member(root, "stereo", "");
    const Value* l = r.member(root, "loudness", "");
    if (d == nullptr || s == nullptr || l == nullptr) return false;
    return r.number(*d, "ratio_step_plr_db", "dynamics.", o.ratio_step_plr_db) &&
           r.number(*d, "ratio_normal", "dynamics.", o.ratio_normal) &&
           r.number(*d, "ratio_high", "dynamics.", o.ratio_high) &&
           r.number(*d, "threshold_above_loudness_db", "dynamics.", o.threshold_above_loudness_db) &&
           r.number(*d, "saturation_plr_db", "dynamics.", o.saturation_plr_db) &&
           r.number(*d, "saturation_drive_db", "dynamics.", o.saturation_drive_db) &&
           r.number(*d, "saturation_mix_pct", "dynamics.", o.saturation_mix_pct) &&
           r.number(*s, "phase_correlation", "stereo.", o.phase_correlation) &&
           r.number(*s, "phase_width_pct", "stereo.", o.phase_width_pct) &&
           r.number(*s, "wide_side_mid_db", "stereo.", o.wide_side_mid_db) &&
           r.number(*s, "wide_width_pct", "stereo.", o.wide_width_pct) &&
           r.number(*s, "near_mono_side_mid_db", "stereo.", o.near_mono_side_mid_db) &&
           r.number(*l, "no_change_lu", "loudness.", o.no_change_lu) &&
           r.number(*l, "dense_limit_db", "loudness.", o.dense_limit_db);
}

}  // namespace

bool parse_mastering_rules(const std::string& json_text, MasteringRules& out, std::string& error) {
    Value root;
    json::Error pe;
    if (!json::parse(json_text, root, pe)) {
        error = "mastering rules: not valid JSON: " + pe.message + " (line " + std::to_string(pe.line) + ")";
        return false;
    }
    Reader r;
    MasteringRules rules;
    const Value* version = r.member(root, "version", "");
    std::int64_t ver = 0;
    if (version != nullptr && (!version->is_number() || !version->to_int64(ver) || ver != 1)) {
        r.error = "mastering rules: unsupported version (this build reads version 1)";
    }
    if (r.error.empty() && read_curve(r, root, rules) && read_groups(r, root, rules) && read_thresholds(r, root, rules)) {
        out = std::move(rules);
        return true;
    }
    error = r.error.empty() ? "mastering rules: malformed" : r.error;
    return false;
}

bool load_mastering_rules(const std::string& path, MasteringRules& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "mastering rules: cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!parse_mastering_rules(ss.str(), out, error)) {
        error += " [" + path + "]";
        return false;
    }
    return true;
}

}  // namespace sonora::analyze
