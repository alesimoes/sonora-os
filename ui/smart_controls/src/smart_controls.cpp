#include "sonora/smart_controls/smart_controls.h"

#include <cmath>
#include <map>
#include <set>

namespace sonora::smart_controls {

float macro_target(const Mapping& m, float v) {
    const float t = m.invert ? 1.0f - v : v;
    return m.lo + t * (m.hi - m.lo);
}

static std::string validate_macro(const Macro& m) {
    if (m.name.empty()) return "a macro has no name";
    std::set<std::pair<std::uint64_t, std::string>> seen;
    for (const auto& p : m.mappings) {
        if (!std::isfinite(p.lo) || !std::isfinite(p.hi))
            return "macro '" + m.name + "': non-finite range for " + p.key;
        if (!seen.insert({p.insert, p.key}).second)
            return "macro '" + m.name + "': duplicate mapping " + p.key;
    }
    return {};
}

std::string validate(const SmartControlSet& set) {
    if (set.macros.size() > kMaxMacros) return "more than 8 macros";
    std::set<std::string> names;
    for (const auto& m : set.macros) {
        auto why = validate_macro(m);
        if (!why.empty()) return why;
        if (!names.insert(m.name).second) return "duplicate macro name '" + m.name + "'";
    }
    return {};
}

bool add_macro(SmartControlSet& set, Macro macro, std::string& why) {
    set.macros.push_back(std::move(macro));
    why = validate(set);
    if (!why.empty()) {
        set.macros.pop_back();
        return false;
    }
    return true;
}

bool apply(const SmartControlSet& set, std::size_t macro, float value,
           ParamSink& sink, ApplyResult& result, std::string& why) {
    if (macro >= set.macros.size()) {
        why = "no such macro";
        return false;
    }
    if (!(value >= 0.0f && value <= 1.0f)) {
        why = "macro value outside 0..1";
        return false;
    }
    std::map<std::uint64_t, std::vector<std::pair<std::string, float>>> by_insert;
    for (const auto& p : set.macros[macro].mappings) {
        if (!sink.has_param(p.insert, p.key)) {
            ++result.skipped;
            continue;
        }
        by_insert[p.insert].push_back({p.key, macro_target(p, value)});
    }
    for (const auto& [insert, params] : by_insert) {
        sink.begin(insert);
        for (const auto& [key, v] : params) {
            sink.set(insert, key, v);
            ++result.applied;
        }
        sink.end(insert);
    }
    return true;
}

bool FakeSink::has_param(std::uint64_t insert, const std::string& key) const {
    for (const auto& k : known)
        if (k.first == insert && k.second == key) return true;
    return false;
}

}  // namespace sonora::smart_controls
