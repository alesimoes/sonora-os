// SPDX-License-Identifier: GPL-3.0-or-later
//
// A session folder with one stereo audio track, one take and one region on
// it, for the elastic tests; no engine session, only the document.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "sonora/model/model.h"
#include "sonora/model/replication.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace elastic_test {

using sonora::model::Frames;
using sonora::model::Id;

struct Rig {
    std::string dir;
    std::unique_ptr<sonora::model::Document> doc;
    Id track = 0, playlist = 0, source = 0, region = 0;
    std::uint16_t channels = 2;
    std::uint32_t rate = 48000;
    std::string take() const { return dir + "/Audio Files/take.wav"; }
    std::string audio_dir() const { return dir + "/Audio Files"; }
};

// The sample at frame i of the take, the same on every channel.
float take_at(std::int64_t i);

// A take of `frames` frames; one region of `length` at `position`, playing
// from take frame `offset`. A gain line with a point at take frame
// `gain_at` when it is >= 0.
bool make_rig(Rig& r, const char* dir, std::int64_t frames, Frames position, Frames length,
              Frames offset, Frames gain_at = -1);
// Another region of the take on the same playlist; its id in `id`.
bool place_region(Rig& r, Frames position, Frames length, Frames offset, Id& id);

const sonora::model::Region* region_of(const Rig& r, Id id);
const sonora::model::Source* source_of(const Rig& r, Id region);
std::string slurp(const std::string& path);
std::size_t files_in(const std::string& dir);

}  // namespace elastic_test
