// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the plugin-panel scenes share (ADR-0057): put a plugin on a slot of
// the demo session, open its panel, and feed the panel the frames a plugin
// would publish. The demo's fake engine sends none of its own, so the scene
// plays the engine's part at the telemetry rate (30 a second at most; 20
// here) and the panel is shown live. The curves and levels are made up for
// the picture: the real ones come from the plugin's audio thread.
#pragma once

#include <QTimer>
#include <cmath>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <memory>
#include <vector>

#include "screenshot_scene.h"

namespace sonora::ui {

using FrameMaker = std::function<api::PluginTelemetry(double seconds)>;

// `catalog_index` -1: the slot already holds the plugin (the demo's own
// inserts). Otherwise the plugin of fake_catalog() at that index goes there.
// ShotContext holds references only, so copies of it outlive the hook.
inline void show_plugin_panel(ShotContext c, int track, int slot, int catalog_index, FrameMaker make) {
    when_open(c, [track, slot, catalog_index, make](ShotContext& open) {
        const ShotContext ctx = open;
        if (catalog_index >= 0) {
            std::string why;
            if (!ctx.inserts.controller().add(track, slot, catalog_index, why))
                std::fprintf(stderr, "sonora-ui: scene: cannot add the plugin: %s\n", why.c_str());
        }
        // The engine answers the AddInsert a moment later.
        QTimer::singleShot(400, &ctx.view, [ctx, track, slot, make]() {
            const SlotView v = ctx.inserts.controller().slot(track, slot);
            if (!v.present) {
                std::fprintf(stderr, "sonora-ui: scene: the insert did not arrive\n");
                return;
            }
            ctx.inserts.openPanel(track, slot);
            auto* timer = new QTimer(&ctx.view);
            timer->setInterval(50);
            auto seconds = std::make_shared<double>(0.0);
            auto sequence = std::make_shared<uint32_t>(0);
            const uint64_t insert = v.insert;
            QObject::connect(timer, &QTimer::timeout, timer, [ctx, insert, make, seconds, sequence]() {
                *seconds += 0.05;
                api::PluginTelemetry f = make(*seconds);
                f.insert = insert;
                f.sequence = ++*sequence;
                ctx.inserts.onTelemetry(f);
            });
            timer->start();
        });
    });
}

// A level that moves like programme material, in dB.
inline float wander(double seconds, double centre, double depth, double rate) {
    return static_cast<float>(centre + depth * std::sin(seconds * rate) +
                              0.4 * depth * std::sin(seconds * rate * 2.7));
}

}  // namespace sonora::ui
