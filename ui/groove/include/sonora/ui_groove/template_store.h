// SPDX-License-Identifier: GPL-3.0-or-later
//
// The operator's saved grooves (ADR-0115): one schema-versioned JSON file in
// the preferences directory, apart from every session, like the preferences
// themselves. A file that cannot be read or is not what this version writes
// is refused with the reason, never read as "no templates": a groove that
// silently vanished would be applied as something else.
#pragma once

#include <string>
#include <vector>

#include "sonora/groove/groove.h"

namespace sonora::ui {

constexpr int kGrooveTemplatesVersion = 1;
constexpr std::size_t kMaxGrooveNameBytes = 64;
constexpr std::size_t kMaxSavedGrooves = 256;

struct NamedGroove {
    std::string name;
    groove::GrooveTemplate groove;
};

// groove-templates.json beside the preferences: the directory of
// $SONORA_PREFERENCES_FILE, else $XDG_CONFIG_HOME/sonora, else
// $HOME/.config/sonora. Empty when none of them is set.
std::string default_groove_templates_file();

class GrooveTemplateStore {
public:
    explicit GrooveTemplateStore(std::string file) : file_(std::move(file)) {}
    const std::string& file() const { return file_; }

    // A missing file is no templates.
    bool load(std::vector<NamedGroove>& out, std::string& why) const;
    bool find(const std::string& name, groove::GrooveTemplate& out, std::string& why) const;
    // Replaces a template of the same name. Refuses to write over a file it
    // cannot read, which would lose what the operator saved before.
    bool save(const std::string& name, const groove::GrooveTemplate& groove,
              std::string& why) const;

private:
    std::string file_;
};

}  // namespace sonora::ui
