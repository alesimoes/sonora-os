// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tempo operations on a tempo map (time.tempo_edit, ADR-0050): set a tempo,
// ramp between two, scale a range, remove a change, set a meter. Each takes
// the map and replaces it with the whole new one, checked against the
// format's bounds (tempo_map_problem), or refuses with the reason and leaves
// the map alone.
//
// A ramp is a staircase of ordinary entries, so the map stays what the format
// says it is and MusicalMap, the click and the grid read it unchanged and
// exactly. Nothing here is on the audio thread: the plan carries frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model {

// A ramp's most steps: two to a beat is enough that none is heard as a jump;
// the cap keeps the session file and the click plan small.
constexpr std::size_t kMaxRampSteps = 128;

// Index of the entry in force at `at` (the last with entry.at <= at; 0
// before the first). `map` is not empty.
std::size_t tempo_entry_in_force(const std::vector<TempoEntry>& map, Frames at);

// The tempo at `at` becomes `bpm`: the entry there changes, else one is
// inserted carrying the meter in force.
bool tempo_set_at(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames at, float bpm,
                  std::string& why);

// Removes the change at `at`; the first entry cannot go.
bool tempo_remove_at(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames at,
                     std::string& why);

// Tempo goes from bpm_from at `from` to bpm_to at `to`, in even steps in
// time, and stays bpm_to from `to` on. Changes inside the range are replaced;
// refused if one of them changes the meter (ramp each side of it).
bool tempo_ramp(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames from, Frames to,
                float bpm_from, float bpm_to, std::string& why);

// Every tempo in [from, to) times `factor`, the tempo from `to` on as it was.
bool tempo_scale(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames from, Frames to,
                 double factor, std::string& why);

// The bar line nearest `near` on the map, in frames. False when the map or
// the position is out of range.
bool nearest_bar_line(const std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames near,
                      Frames& out);

// A meter change starts a bar, so it goes on the bar line nearest `near`,
// which is returned in `at`. The tempo there is the one in force.
bool meter_set_near(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames near,
                    std::uint32_t beats, std::uint32_t note, Frames& at, std::string& why);

}  // namespace sonora::model
