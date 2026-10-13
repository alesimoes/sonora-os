// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0006: the UI "determines its rendering path at start-up and says which
// one it got, loudly". A machine that quietly fell back to software
// rasterisation and then cost the callback CPU mid-take is the failure this
// exists to prevent.
#pragma once

#include <QString>

class QQuickWindow;

namespace sonora::ui {

class ShellModel;

struct RenderPath {
    QString api;          // "OpenGL", "Vulkan", "software scene graph", ...
    QString device;       // the driver's own name for the renderer
    bool software = false;          // a CPU rasteriser is doing the drawing
    bool meters_available = true;   // false on the software scene graph
    QString describe() const;
};

// True for the CPU rasterisers Mesa and others ship: llvmpipe, softpipe,
// lavapipe, swrast, SwiftShader, Microsoft's WARP.
bool is_software_rasterizer(const QString& device_name);

// Hooks `window` so that when its scene graph is initialised the path is
// read on the render thread, printed to stderr, and handed to `model` on
// the GUI thread. `on_known` runs after that, on the GUI thread.
void probe_render_path(QQuickWindow* window, ShellModel* model,
                       void (*on_known)(const RenderPath&, void*), void* ctx);

}  // namespace sonora::ui
