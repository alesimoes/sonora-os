// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which key map profiles the operator can switch between, and which one the
// interface comes back on after a restart.
//
// The profiles are the JSON files in one folder (keymaps/ in the tree,
// share/sonora/keymaps when installed). Each is loaded when the list is made,
// so a file that would be refused is shown with the reason instead of being
// offered as a choice that then does nothing. The choice is remembered in
// $XDG_CONFIG_HOME/sonora/keymap (else ~/.config/sonora/keymap), beside the
// device choice (ADR-0007 M2 note 8): on the appliance systemd restarts the
// interface, and a profile that reverted on every restart would be a switch
// that lies.
//
// No Qt here: a test can check it without a display.
#pragma once

#include <string>
#include <vector>

#include "sonora/keymap.h"

namespace sonora::ui {

// `file` as every profile path here is compared: absolute and lexically
// normal, so "keymaps/./protools.json" and "x/../keymaps/protools.json" name
// the same entry. "" stays "" (the built-in default). Symlinks are not
// resolved: two links to one file stay two entries, as list_profiles
// deduplicates them separately (fs::equivalent).
std::string profile_path(const std::string& file);

struct ProfileEntry {
    // profile_path(); empty for the built-in default.
    std::string file;
    // The profile's "name", or the file name when it does not load.
    std::string name;
    // Why the file is refused; empty when it loads.
    std::string error;
};

// The profiles in `dir`, sorted by name, each loaded once to fill `error`.
// The built-in default comes first unless a loadable file in `dir` carries
// its name (keymaps/default.json does). `extra`, a --keymap path outside
// `dir`, is listed too. A missing or unreadable `dir` gives the built-in
// default alone.
std::vector<ProfileEntry> list_profiles(const std::string& dir, const std::string& extra = "");

// The folder the profiles are read from: `given` (--keymap-dir) when set;
// else $SONORA_KEYMAP_DIR; else the folder of `keymap_path` (--keymap);
// else `exe_dir`/../share/sonora/keymaps; else /usr/share/sonora/keymaps.
// The first that exists, or "" when none does.
std::string keymap_dir(const std::string& given, const std::string& keymap_path,
                       const std::string& exe_dir);

// The remembered choice file, under `config_home` when it is set, else
// $XDG_CONFIG_HOME, else $HOME/.config. "" when none of them is known. A
// relative $XDG_CONFIG_HOME is ignored, as the XDG Base Directory spec says:
// under systemd the cwd is /, and the choice would land there.
std::string remembered_profile_path(const std::string& config_home = "");

// The profile remembered at `path`: the file it names, made absolute against
// `dir` when it is a bare file name; "" for the built-in default or when
// nothing is remembered.
std::string remembered_profile(const std::string& path, const std::string& dir);

// Records `file` (a profile path, "" for the built-in default) at `path`,
// as a bare file name when it lies in `dir`. Written to a temporary file and
// renamed, so a power cut leaves the old choice or the new one. False, with
// `why`, when it could not be written.
bool remember_profile(const std::string& path, const std::string& dir, const std::string& file,
                      std::string& why);

// The profile the interface starts on: `keymap_arg` (--keymap) when given;
// else the one remembered at `remembered_at`; else the built-in default. A
// profile that does not load is refused whole and the built-in default runs
// instead (keymap_or_built_in), `refusal` saying which and why, and `file`
// empty so the Keys menu does not mark a profile that is not running.
struct StartupProfile {
    KeyMap map;
    std::string file;      // profile_path(); "" for the built-in default
    std::string refusal;   // "" when nothing was refused
};
StartupProfile startup_profile(const std::string& keymap_arg, const std::string& remembered_at,
                               const std::string& dir);

}  // namespace sonora::ui
