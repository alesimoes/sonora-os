// SPDX-License-Identifier: GPL-3.0-or-later
//
// The punch range from the UI, in the Pro Tools profile: the Edit selection
// made the punch range through transport.punch_selection, as the bar's PUNCH
// chip sends it, and a pre-roll of 1.5 s typed into the Transport window.
// PUNCH lit on the bar, the range in the window in the main counter's
// format, the rolls live with the line saying where they apply.
//
// Both are engine commands. Sent before the fake engine has answered the
// demo's load, which on a loaded machine can be later than the scene's
// hook, they land on the document the load then replaces; so what the
// replica shows is checked a little later, and what is missing sent again.
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void punch_and_roll(SessionModel& session, ShellModel& shell, int tries) {
    const auto& settings = session.controller().snapshot().settings;
    const bool punched = settings.transport.punch[1] > settings.transport.punch[0];
    const bool rolled = settings.transport.pre_roll == settings.sample_rate * 3 / 2;
    if (punched && rolled) return;
    if (tries <= 0) {
        std::fprintf(stderr, "sonora-ui: transport-punch: the engine did not take %s\n",
                     punched ? "the pre-roll" : "the punch range");
        return;
    }
    if (!punched) shell.invoke(QStringLiteral("transport.punch_selection"), -1);
    if (!rolled) session.setPreRollText(QStringLiteral("1.5"));
    QTimer::singleShot(250, QCoreApplication::instance(), [&session, &shell, tries]() {
        punch_and_roll(session, shell, tries - 1);
    });
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
        punch_and_roll(c.session, c.model, 12);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
