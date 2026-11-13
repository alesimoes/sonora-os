// SPDX-License-Identifier: GPL-3.0-or-later
//
// Elastic Audio as tempo.edit words (ADR-0136): the selected region is
// stretched by a ratio, along warp markers, or onto a beat grid (Quantize
// Audio), by the engine's audio.elastic, which renders a new file and
// repoints the region in one undo step. Built on EditController's public
// API only; the words reach it through tempo.edit ("elastic ...",
// ui/qt/src/edit_tempo.cpp).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sonora/api/elastic.h"

namespace sonora::ui {

class EditController;

struct ElasticDeps {
    // How audio.elastic leaves the interface (the shell's command sender).
    // Empty: refused, since nothing would run the request.
    std::function<void(const std::string& command, const std::string& argument)> send;
    // Hello's capabilities. Null or without audio.elastic: refused, as an
    // engine built without the stretcher answers NotImplemented (ADR-0136 §3).
    const std::vector<std::string>* capabilities = nullptr;
};

// The words after "elastic", for the selected region:
//   ratio <R> [algorithm]                     new length / old, 0.25..4
//   quantize <grid> [strength] [algorithm]    grid in beats (0.25) or a note (1/16),
//                                             strength 1..100 percent, default 100
//   warp <SRC:DST,...> [algorithm]            frames from the region's start
// The algorithm is polyphonic (the default), rhythmic, monophonic or varispeed.
// The request is checked here as the engine checks it (contract ranges, the
// region's length) and, where the length is known, against the next region:
// playlists never overlap, so a stretch into it is refused, not rippled.
bool elastic_request(const EditController& ec, const std::string& text, api::ElasticRequest& out,
                     std::string& why);

// Whether `deps` can run audio.elastic at all; the reason when not.
bool elastic_available(const ElasticDeps& deps, std::string& why);

// Checks, then sends one audio.elastic. True means sent: the render and its
// refusals (a take open for writing, no onsets) come back as the engine's
// reply, which the shell holds as a notice like every engine refusal.
bool elastic_command(EditController& ec, const ElasticDeps& deps, const std::string& text,
                     std::string& why);

// The tempo.edit entry point. EditController has no route to the engine, so
// this checks the words and refuses with what is missing rather than appear
// to stretch: a control that pretends is worse than one that refuses.
bool elastic_command(EditController& ec, const std::string& text, std::string& why);

}  // namespace sonora::ui
