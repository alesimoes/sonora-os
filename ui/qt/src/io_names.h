// SPDX-License-Identifier: GPL-3.0-or-later
//
// The names Setup > I/O Setup gives the interface's hardware inputs and
// outputs (Pro Tools' I/O Setup, Input and Output pages): "Kick In" for
// input 1 of the UMC1820, so a track picks its input by what is plugged in
// rather than by a number.
//
// The names belong to the machine, not to a session: they say what is
// cabled to this interface, which does not change when another session is
// opened. They are kept in $XDG_CONFIG_HOME/sonora/io-names (else
// ~/.config/sonora/io-names), beside the key map choice, so a restart of
// the interface by systemd does not forget them. Only names that differ
// from the default are written; an input the file does not name is "In N".
//
// How many inputs there are is the engine's to say (the state block's
// inputs_connected), never this file's: a name kept for input 19 of an
// interface that has 18 is kept, and not shown.
//
// No Qt here: a test can check it without a display.
#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace sonora::ui {

enum class IoSide { Input, Output };

// Pro Tools' own defaults for a mono path: "In 1", "Out 2". `index` counts
// from 0.
std::string default_io_name(IoSide side, std::size_t index);

class IoNames {
public:
    // The name of input or output `index` (from 0): the one given, else the
    // default.
    std::string name(IoSide side, std::size_t index) const;
    bool renamed(IoSide side, std::size_t index) const;

    // Why `name` cannot name input or output `index` of `count`, or empty
    // when it can. Refused rather than rewritten: empty, longer than
    // kMaxLength, a tab or a line break (the file's separators), or the name
    // another of the first `count` on the same side already shows, which
    // would make two entries of the input picker read the same.
    static constexpr std::size_t kMaxLength = 32;
    std::string problem(IoSide side, std::size_t index, std::size_t count,
                        const std::string& name) const;

    // Gives `index` the name, or takes it back to its default when `name` is
    // the default. Does not check: problem() first.
    void set(IoSide side, std::size_t index, const std::string& name);
    void clear() {
        inputs_.clear();
        outputs_.clear();
    }

    // "in\t<n>\t<name>" and "out\t<n>\t<name>" lines, n from 1. A line that
    // does not parse is skipped, so a hand-edited file loses that line and
    // not the rest.
    std::string serialize() const;
    static IoNames parse(const std::string& text);

    bool operator==(const IoNames& o) const {
        return inputs_ == o.inputs_ && outputs_ == o.outputs_;
    }

private:
    const std::map<std::size_t, std::string>& side(IoSide s) const {
        return s == IoSide::Input ? inputs_ : outputs_;
    }
    std::map<std::size_t, std::string>& side(IoSide s) {
        return s == IoSide::Input ? inputs_ : outputs_;
    }
    std::map<std::size_t, std::string> inputs_;
    std::map<std::size_t, std::string> outputs_;
};

// The names file, under `config_home` when it is set, else $XDG_CONFIG_HOME
// (ignored when relative), else $HOME/.config. "" when none is known.
std::string io_names_path(const std::string& config_home = "");

// The names kept at `path`; none when the file is not there.
IoNames load_io_names(const std::string& path);

// Written to a temporary file and renamed, so a power cut leaves the old
// names or the new ones. False, with `why`, when it could not be written.
bool save_io_names(const std::string& path, const IoNames& names, std::string& why);

}  // namespace sonora::ui
