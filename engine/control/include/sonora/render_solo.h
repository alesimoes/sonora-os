// SPDX-License-Identifier: GPL-3.0-or-later
//
// The render-scoped solo of one insert (ADR-0087 §4): for a Process
// Selection through one plug-in, the track's other active inserts are
// silenced on their hosts for the render, never in the document, and the
// hosts are returned to the document's bypass on every exit. PluginHosting
// is the engine's; RenderInPlace only sees this seam, so its tests fake it.
// Control thread only: the override is posted like a bypass change.
#pragma once

#include "sonora/model/model.h"

namespace sonora {

class RenderSolo {
public:
    virtual ~RenderSolo() = default;
    // Posts the override the first time: every active insert of `track`
    // other than `insert` bypassed on its host. True once every host reads
    // it back (what the running host's bypass table says); called again
    // until then, it re-sends what the session could not take and never
    // waits. False for a track without a running chain or an `insert` it
    // does not run.
    virtual bool render_solo(model::Id track, model::Id insert) = 0;
    // Lifts the override: the document's bypass republished, forced, to
    // every insert of the track. Harmless without one.
    virtual void render_unsolo(model::Id track) = 0;
};

}  // namespace sonora
