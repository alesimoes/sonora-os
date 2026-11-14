// SPDX-License-Identifier: GPL-3.0-or-later
//
// Coverage: every shortcut the vendors document (key and command name only,
// as brain/research/protools-logic-key-bindings.md records them) whose
// command exists in the catalogue is bound in the profile. A documented
// shortcut whose command does not exist yet is listed, not failed: the day
// the command lands in command.h this test demands its binding, so a new
// command cannot ship with its documented key forgotten.
#include "sonora/keymap.h"

#include <cstdio>
#include <string>

namespace {

struct Doc {
    const char* file;
    const char* chord;
    const char* command;
};

const Doc kDocumented[] = {
    // Pro Tools: transport
    {"protools.json", "space", "transport.toggle_play"},
    {"protools.json", "shift+space", "transport.play_half_speed"},
    {"protools.json", "ctrl+space", "transport.record"},
    {"protools.json", "f12", "transport.record"},
    {"protools.json", "return", "transport.return_to_zero"},
    {"protools.json", "alt+return", "transport.go_to_end"},
    {"protools.json", "numpad_0", "transport.toggle_play"},
    {"protools.json", "numpad_1", "transport.rewind"},
    {"protools.json", "numpad_2", "transport.fast_forward"},
    {"protools.json", "numpad_3", "transport.record_ready"},
    {"protools.json", "numpad_4", "transport.toggle_loop"},
    {"protools.json", "ctrl+shift+l", "transport.toggle_loop"},
    // ADR-0043, ADR-0129: the record-mode switches.
    {"protools.json", "alt+k", "transport.toggle_input_only"},
    {"protools.json", "ctrl+k", "transport.toggle_pre_post_roll"},
    {"protools.json", "ctrl+shift+p", "transport.toggle_quick_punch"},
    {"protools.json", "ctrl+shift+t", "track.toggle_track_punch"},
    // Pro Tools: edit modes, with the Option+digit alternates
    {"protools.json", "f1", "edit.mode.shuffle"},
    {"protools.json", "f2", "edit.mode.slip"},
    {"protools.json", "f3", "edit.mode.spot"},
    {"protools.json", "f4", "edit.mode.grid"},
    {"protools.json", "alt+1", "edit.mode.shuffle"},
    {"protools.json", "alt+2", "edit.mode.slip"},
    {"protools.json", "alt+3", "edit.mode.spot"},
    {"protools.json", "alt+4", "edit.mode.grid"},
    {"protools.json", "grave", "edit.mode.next"},
    {"protools.json", "shift+f4", "edit.toggle_snap"},
    // Pro Tools: tools, with the Command+digit alternates
    {"protools.json", "f5", "tool.zoom"},
    {"protools.json", "f6", "tool.trim"},
    {"protools.json", "f7", "tool.select"},
    {"protools.json", "f8", "tool.grab"},
    {"protools.json", "f9", "tool.scrub"},
    {"protools.json", "f10", "tool.pencil"},
    {"protools.json", "ctrl+1", "tool.zoom"},
    {"protools.json", "ctrl+2", "tool.trim"},
    {"protools.json", "ctrl+3", "tool.select"},
    {"protools.json", "ctrl+4", "tool.grab"},
    {"protools.json", "ctrl+5", "tool.scrub"},
    {"protools.json", "ctrl+6", "tool.pencil"},
    {"protools.json", "ctrl+7", "tool.smart"},
    {"protools.json", "escape", "tool.next"},
    // Pro Tools: zoom, nudge
    {"protools.json", "ctrl+bracketright", "zoom.in"},
    {"protools.json", "ctrl+bracketleft", "zoom.out"},
    {"protools.json", "ctrl+alt+bracketright", "zoom.vertical_in"},
    {"protools.json", "ctrl+alt+bracketleft", "zoom.vertical_out"},
    {"protools.json", "alt+f", "zoom.fit_selection"},
    {"protools.json", "alt+a", "zoom.fit_session"},
    {"protools.json", "numpad_plus", "nudge.forward"},
    {"protools.json", "numpad_minus", "nudge.back"},
    // Pro Tools: edit and file menus
    {"protools.json", "ctrl+z", "edit.undo"},
    {"protools.json", "ctrl+shift+z", "edit.redo"},
    {"protools.json", "ctrl+x", "edit.cut"},
    {"protools.json", "ctrl+c", "edit.copy"},
    {"protools.json", "ctrl+v", "edit.paste"},
    {"protools.json", "ctrl+a", "edit.select_all"},
    {"protools.json", "ctrl+d", "edit.duplicate"},
    {"protools.json", "ctrl+e", "edit.separate_at_selection"},
    {"protools.json", "ctrl+h", "edit.heal_separation"},
    {"protools.json", "ctrl+b", "edit.clear"},
    {"protools.json", "alt+r", "edit.repeat"},
    {"protools.json", "alt+shift+3", "edit.consolidate"},
    {"protools.json", "ctrl+s", "app.save"},
    {"protools.json", "alt+shift+d", "track.duplicate"},
    {"protools.json", "shift+r", "track.toggle_arm"},
    {"protools.json", "shift+s", "track.toggle_solo"},
    {"protools.json", "shift+m", "track.toggle_mute"},
    {"protools.json", "shift+i", "track.toggle_input_monitor"},
    // Pro Tools: documented, command not in the catalogue yet
    {"protools.json", "ctrl+t", "edit.trim_to_selection"},
    {"protools.json", "ctrl+shift+e", "edit.insert_silence"},
    {"protools.json", "ctrl+f", "edit.fade_dialog"},
    {"protools.json", "ctrl+m", "clip.toggle_mute"},
    {"protools.json", "ctrl+shift+p", "transport.toggle_quickpunch"},

    // Logic Pro: transport
    {"logic.json", "space", "transport.toggle_play"},
    {"logic.json", "r", "transport.record"},
    {"logic.json", "return", "transport.return_to_zero"},
    {"logic.json", "alt+return", "transport.go_to_end"},
    {"logic.json", "c", "transport.toggle_cycle"},
    {"logic.json", "comma", "transport.rewind"},
    {"logic.json", "period", "transport.fast_forward"},
    {"logic.json", "numpad_0", "transport.stop"},
    {"logic.json", "numpad_enter", "transport.play"},
    {"logic.json", "numpad_multiply", "transport.punch"},
    {"logic.json", "cmd+u", "transport.cycle_selection"},
    // Logic Pro: edit, zoom, nudge, tracks
    {"logic.json", "cmd+z", "edit.undo"},
    {"logic.json", "cmd+shift+z", "edit.redo"},
    {"logic.json", "cmd+x", "edit.cut"},
    {"logic.json", "cmd+c", "edit.copy"},
    {"logic.json", "cmd+v", "edit.paste"},
    {"logic.json", "cmd+a", "edit.select_all"},
    {"logic.json", "cmd+s", "app.save"},
    {"logic.json", "cmd+q", "app.quit"},
    {"logic.json", "cmd+t", "edit.separate_at_selection"},
    {"logic.json", "cmd+j", "edit.heal_separation"},
    {"logic.json", "cmd+r", "edit.repeat"},
    {"logic.json", "delete", "edit.clear"},
    {"logic.json", "ctrl+b", "edit.consolidate"},
    {"logic.json", "cmd+[", "edit.trim_start_to_insertion"},
    {"logic.json", "cmd+]", "edit.trim_end_to_insertion"},
    {"logic.json", "ctrl+alt+x", "edit.fade_selection"},
    {"logic.json", "alt+pagedown", "tool.next"},
    {"logic.json", "z", "zoom.fit_selection"},
    {"logic.json", "cmd+right", "zoom.in"},
    {"logic.json", "cmd+left", "zoom.out"},
    {"logic.json", "cmd+down", "zoom.vertical_in"},
    {"logic.json", "cmd+up", "zoom.vertical_out"},
    {"logic.json", "alt+right", "nudge.forward"},
    {"logic.json", "alt+left", "nudge.back"},
    {"logic.json", "m", "track.toggle_mute"},
    {"logic.json", "s", "track.toggle_solo"},
    {"logic.json", "ctrl+r", "track.toggle_arm"},
    {"logic.json", "ctrl+i", "track.toggle_input_monitor"},
    {"logic.json", "up", "track.select_previous"},
    {"logic.json", "down", "track.select_next"},
    // Logic Pro: documented, command not in the catalogue yet
    {"logic.json", "cmd+g", "edit.toggle_snap"},
    {"logic.json", "alt+pageup", "tool.previous"},
    {"logic.json", "q", "edit.quantize"},
    {"logic.json", "l", "region.toggle_loop"},
    {"logic.json", "ctrl+m", "clip.toggle_mute"},
    {"logic.json", "shift+period", "transport.rewind_fast"},
};

}  // namespace

int main() {
    using namespace sonora;
    int failures = 0;
    int checked = 0;
    int pending = 0;
    for (const Doc& d : kDocumented) {
        if (find_command(d.command) == nullptr) {
            ++pending;
            std::printf("pending: %s %s -> %s (command not in the catalogue)\n",
                        d.file, d.chord, d.command);
            continue;
        }
        KeyMap map;
        std::string err;
        const std::string path = std::string(SONORA_KEYMAP_DIR) + "/" + d.file;
        if (!KeyMap::load_file(path, map, err)) {
            std::fprintf(stderr, "FAIL %s does not load: %s\n", path.c_str(),
                         err.c_str());
            return 1;
        }
        const Binding* b = map.resolve(std::string(d.chord));
        ++checked;
        if (b == nullptr || b->command != d.command) {
            std::fprintf(stderr, "FAIL %s: documented %s -> %s is %s\n", d.file,
                         d.chord, d.command,
                         b == nullptr ? "unbound" : b->command.c_str());
            ++failures;
        }
    }
    // ADR-0190: our own chord, so not in the vendor lists above: it must
    // resolve to the reset, with its argument, in both profiles.
    for (const char* file : {"protools.json", "logic.json"}) {
        KeyMap map;
        std::string err;
        const std::string path = std::string(SONORA_KEYMAP_DIR) + "/" + file;
        if (!KeyMap::load_file(path, map, err)) {
            std::fprintf(stderr, "FAIL %s does not load: %s
", path.c_str(), err.c_str());
            return 1;
        }
        const Binding* b = map.resolve("ctrl+alt+0");
        if (find_command("transport.varispeed") == nullptr || b == nullptr ||
            b->command != "transport.varispeed" || b->ctx.argument != "1") {
            std::fprintf(stderr, "FAIL %s: ctrl+alt+0 is not the varispeed reset
", file);
            ++failures;
        }
    }
    std::printf("documented shortcuts checked: %d, pending a command: %d\n",
                checked, pending);
    return failures == 0 ? 0 : 1;
}
