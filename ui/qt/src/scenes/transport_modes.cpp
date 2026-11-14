// SPDX-License-Identifier: GPL-3.0-or-later
//
// The record-mode switches in the Pro Tools profile (ADR-0043, ADR-0129):
// QuickPunch and Input Only on, Pre/Post-Roll off, each sent as its key
// sends it. The Transport window lights each switch from the session's field
// and the bar shows the QUICKPUNCH and INPUT ONLY chips.
//
// The commands are the engine's. Sent before the fake engine has answered the
// demo's load they land on the document the load replaces, so what the
// replica shows is checked a little later and what is missing sent again.
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void set_modes(SessionModel& session, ShellModel& shell, int tries) {
    const auto& t = session.controller().snapshot().settings.transport;
    const bool quick = t.quick_punch, input = t.input_only, rolls_off = !t.rolls_on;
    if (quick && input && rolls_off) return;
    if (tries <= 0) {
        std::fprintf(stderr, "sonora-ui: transport-modes: the engine did not take every switch\n");
        return;
    }
    if (!quick) shell.invoke(QStringLiteral("transport.toggle_quick_punch"), -1);
    if (!input) shell.invoke(QStringLiteral("transport.toggle_input_only"), -1);
    if (!rolls_off) shell.invoke(QStringLiteral("transport.toggle_pre_post_roll"), -1);
    QTimer::singleShot(250, QCoreApplication::instance(), [&session, &shell, tries]() {
        set_modes(session, shell, tries - 1);
    });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-modes");
    s.args = QStringLiteral("--keymap keymaps/protools.json");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) { set_modes(c.session, c.model, 12); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
