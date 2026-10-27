// SPDX-License-Identifier: GPL-3.0-or-later
//
// The punch range from the UI, in the Pro Tools profile: the Edit selection
// made the punch range through transport.punch_selection, as the bar's PUNCH
// chip sends it, and a pre-roll of 1.5 s typed into the Transport window.
// PUNCH lit on the bar, the range in the window in the main counter's
// format, the rolls live with the line saying where they apply.
//
// Both are edits through the replica, which refuses them until the fake
// engine has answered the demo's load; on a loaded machine that can be later
// than the scene's hook, so a refused edit is tried again a little later.
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <string>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void punch_and_roll(SessionModel& session, int tries) {
    std::string why;
    EditController& edit = session.controller();
    const auto& settings = edit.snapshot().settings;
    const model::Frames pre = settings.sample_rate * 3 / 2;
    const bool punched = settings.transport.punch[1] > settings.transport.punch[0] ||
                         edit.set_punch_from_selection(why);
    const bool rolled =
        punched && (edit.snapshot().settings.transport.pre_roll == pre || edit.set_pre_roll(pre, why));
    // Republishes what changed, as a key's edit does.
    session.reportEdit(true, why);
    if (punched && rolled) return;
    if (tries <= 0) {
        std::fprintf(stderr, "sonora-ui: transport-punch: %s\n", why.c_str());
        return;
    }
    QTimer::singleShot(250, QCoreApplication::instance(),
                       [&session, tries]() { punch_and_roll(session, tries - 1); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-punch");
    s.args = QStringLiteral("--keymap keymaps/protools.json");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 2) return;
        c.session.controller().select_range(c.track, p->regions[1].position, p->regions[2].end());
        punch_and_roll(c.session, 12);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
