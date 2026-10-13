// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/tempo_map.h"

#include <cmath>
#include <limits>

#include "check.h"

using namespace sonora::midi;

namespace {

int constant_tempo() {
    const TempoMap map(960);  // 120 BPM
    CHECK(map.points().size() == 1 && map.usec_per_quarter_at(12345) == 500000);
    CHECK(map.ticks_to_samples(0, 48000) == 0);
    CHECK(map.ticks_to_samples(960, 48000) == 24000);    // a beat is 0.5 s
    CHECK(map.ticks_to_samples(3840, 44100) == 88200);   // a 4/4 bar is 2 s
    CHECK(map.ticks_to_samples(1, 48000) == 25);
    CHECK(map.samples_to_ticks(24000, 48000) == 960);
    CHECK(std::fabs(map.seconds_at(1920) - 1.0) < 1e-12);

    // Rounding to the nearest sample: 1 tick at 44.1 kHz is 22.96875 samples.
    CHECK(map.ticks_to_samples(1, 44100) == 23);
    CHECK(map.ticks_to_samples(32, 44100) == 735);

    // Rounding to the nearest tick from samples.
    CHECK(map.samples_to_ticks(12, 48000) == 0);
    CHECK(map.samples_to_ticks(13, 48000) == 1);
    CHECK(map.samples_to_ticks(37, 48000) == 1);
    CHECK(map.samples_to_ticks(38, 48000) == 2);

    // Before 0, the first tempo extrapolates.
    CHECK(map.ticks_to_samples(-960, 48000) == -24000);
    CHECK(map.samples_to_ticks(-24000, 48000) == -960);
    return 0;
}

int changing_tempo() {
    TempoMap map(960);
    map.set_tempo(1920, 1000000);  // 60 BPM from beat 3
    map.set_tempo(3840, 250000);   // 240 BPM from beat 5
    CHECK(map.points().size() == 3);
    CHECK(map.usec_per_quarter_at(1919) == 500000);
    CHECK(map.usec_per_quarter_at(1920) == 1000000);
    CHECK(map.usec_per_quarter_at(5000) == 250000);

    // 2 beats at 120 (1 s), then 2 beats at 60 (2 s), then 240.
    CHECK(map.ticks_to_samples(1920, 48000) == 48000);
    CHECK(map.ticks_to_samples(2880, 48000) == 96000);
    CHECK(map.ticks_to_samples(3840, 48000) == 144000);
    CHECK(map.ticks_to_samples(4800, 48000) == 156000);
    CHECK(std::fabs(map.seconds_at(4800) - 3.25) < 1e-12);
    CHECK(map.samples_to_ticks(156000, 48000) == 4800);
    CHECK(map.samples_to_ticks(96000, 48000) == 2880);

    // Replacing a tempo at an existing tick does not add a point.
    map.set_tempo(1920, 750000);
    CHECK(map.points().size() == 3 && map.usec_per_quarter_at(1920) == 750000);
    CHECK(map.ticks_to_samples(3840, 48000) == 48000 + 72000);

    // A tempo at 0 replaces the default.
    map.set_tempo(0, 600000);
    CHECK(map.points().size() == 3 && map.points()[0].usec_per_quarter == 600000);
    return 0;
}

int round_trip_every_tick() {
    TempoMap map(960);
    map.set_tempo(1000, 428571);   // 140 BPM, not a round number of samples
    map.set_tempo(2500, 652174);   // 92 BPM
    map.set_tempo(7000, 300000);   // 200 BPM: 15 samples per tick at 48 kHz
    for (const std::uint32_t rate : {44100u, 48000u, 96000u, 192000u}) {
        std::int64_t prev = std::numeric_limits<std::int64_t>::min();
        for (Tick t = -50; t < 12000; ++t) {
            const std::int64_t s = map.ticks_to_samples(t, rate);
            CHECK(s > prev);  // strictly increasing
            prev = s;
            CHECK(map.samples_to_ticks(s, rate) == t);
        }
    }
    return 0;
}

int long_positions_stay_exact() {
    // Three hours at 120 BPM and 960 ppq, at 192 kHz: no drift, because
    // nothing is accumulated in floating point.
    const TempoMap map(960);
    const Tick ticks = Tick{3} * 3600 * 2 * 960;
    const std::int64_t samples = std::int64_t{3} * 3600 * 192000;
    CHECK(map.ticks_to_samples(ticks, 192000) == samples);
    CHECK(map.samples_to_ticks(samples, 192000) == ticks);
    CHECK(map.ticks_to_samples(ticks + 1, 192000) == samples + 100);
    return 0;
}

int from_events_and_bpm() {
    EventList events{note_on(0, 0, 60, 1), tempo(1920, 1000000), tempo(0, 400000),
                     tempo(1920, 800000)};
    const TempoMap map = TempoMap::from_events(480, events);
    CHECK(map.ppq() == 480);
    CHECK(map.points().size() == 2);
    CHECK(map.points()[0] == (TempoPoint{0, 400000}));
    // Later in the list wins at the same tick, as a player would apply them.
    CHECK(map.points()[1] == (TempoPoint{1920, 800000}));

    CHECK(TempoMap::usec_from_bpm(120.0) == 500000);
    CHECK(TempoMap::usec_from_bpm(140.0) == 428571);
    CHECK(std::fabs(TempoMap::bpm(500000) - 120.0) < 1e-9);
    CHECK(TempoMap::usec_from_bpm(0.0) == 0xFFFFFF);
    CHECK(TempoMap::usec_from_bpm(1e9) == 1);

    // Clamped construction.
    CHECK(TempoMap(0).ppq() == 1 && TempoMap(100000).ppq() == 32767);
    return 0;
}

}  // namespace

int main() {
    RUN(constant_tempo);
    RUN(changing_tempo);
    RUN(round_trip_every_tick);
    RUN(long_positions_stay_exact);
    RUN(from_events_and_bpm);
    std::printf("midi_tempo_map: all tests passed\n");
    return 0;
}
