// SPDX-License-Identifier: GPL-3.0-or-later
// How the tests reach the rule table: the shipped data/mastering_rules.json,
// loaded once. A failure to load is fatal, as it is for the CLI.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sonora/analyze/mastering_rules.h"

inline const sonora::analyze::MasteringRules& shipped_rules() {
    static const sonora::analyze::MasteringRules rules = [] {
        sonora::analyze::MasteringRules r;
        std::string err;
        if (!sonora::analyze::load_mastering_rules(RULES_PATH, r, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            std::exit(1);
        }
        return r;
    }();
    return rules;
}

inline sonora::analyze::MasteringConfig with_rules(sonora::analyze::MasteringConfig c = {}) {
    c.table = &shipped_rules();
    return c;
}

inline bool run_suggest(const sonora::analyze::MixAnalysis& a, const sonora::analyze::MasteringConfig& c,
                        sonora::analyze::MasteringSuggestion& out, std::string& err) {
    return sonora::analyze::suggest_mastering(a, with_rules(c), out, err);
}
