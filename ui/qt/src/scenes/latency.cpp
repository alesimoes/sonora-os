// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Latency (ADR-0019, update of 2026-10-26) on the demo's loopback
// cable: the window opened through window.latency, out_2 into the last
// input measured through latency.measure, as Measure sends it. The shot is
// taken seconds later, with the fake's 1.5 s measurement done.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("latency");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("window.latency"), -1);
        c.model.measureLatency(2, c.model.trackCount());
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
