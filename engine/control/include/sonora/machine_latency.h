// SPDX-License-Identifier: GPL-3.0-or-later
//
// The machine's round trip L, kept between runs and never in a session
// file (ADR-0019, update of 2026-10-26): one line per capture device, rate
// and period in latency.conf in the user's config folder, since the round
// trip changes with each. Control thread and start-up only: files.
#pragma once

#include <cstdint>
#include <string>

#include "sonora/api/messages.h"

namespace sonora {

struct LatencyKey {
    std::string device;   // AudioBackend::capture_device()
    uint32_t rate = 0;
    uint32_t period = 0;  // AudioBackend::buffer_frames()
};

// What is kept for a key: Measured or Manual, never None or Backend, which
// are worked out again at every start.
struct KeptLatency {
    uint32_t frames = 0;
    api::LatencySource source = api::LatencySource::None;
};

// `config_dir`/latency.conf; empty when `config_dir` is.
std::string machine_latency_file(const std::string& config_dir);

// The figure kept for `key`. False when the file, or a valid line for the
// key, is not there.
bool load_machine_latency(const std::string& path, const LatencyKey& key, KeptLatency& out);

// Keeps `kept` for `key`, replacing that key's line and leaving the others,
// by a temporary file and a rename, so a crash leaves the old file or the
// new one. Creates the folder. False, with `why`, when it cannot.
bool save_machine_latency(const std::string& path, const LatencyKey& key, const KeptLatency& kept,
                          std::string& why);

}  // namespace sonora
