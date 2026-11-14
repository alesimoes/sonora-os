// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Warp panel's marker model (ADR-0136 §1): markers stay in order on both
// sides whatever the operator does, every refused edit says why and changes
// nothing, a new region drops the old markers, and what the panel lists is
// what the words send.
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/api/elastic.h"
#include "sonora/ui_elastic/warp_markers.h"

using namespace sonora;
using namespace sonora::ui;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

constexpr std::int64_t kLength = 96000;  // two seconds at 48 kHz

bool mentions(const std::string& text, const char* part) {
    const bool found = text.find(part) != std::string::npos;
    if (!found) std::fprintf(stderr, "  \"%s\" does not mention \"%s\"\n", text.c_str(), part);
    return found;
}

using Markers = std::vector<api::WarpMarker>;

void placing_keeps_order() {
    WarpMarkers m;
    std::string why;
    CHECK(!m.add(100, 100, why) && mentions(why, "select a region"));

    m.reset(7, kLength);
    CHECK(m.region() == 7 && m.length() == kLength && m.empty());
    CHECK(m.output_length() == kLength);
    // Out of order as placed, in order as held.
    CHECK(m.add(48000, 60000, why));
    CHECK(m.add(24000, 24000, why));
    CHECK(m.add(72000, 84000, why));
    CHECK((m.markers() == Markers{{24000, 24000}, {48000, 60000}, {72000, 84000}}));
    // What follows the last marker keeps its speed.
    CHECK(m.output_length() == 84000 + (kLength - 72000));
    CHECK(m.speed(0) == 1.0);
    CHECK(m.speed(1) == 1.5);
    CHECK(m.speed(2) == 1.0);
}

void refused_edits_change_nothing() {
    WarpMarkers m;
    m.reset(7, kLength);
    std::string why;
    CHECK(m.add(48000, 60000, why));
    const Markers before = m.markers();

    CHECK(!m.add(0, 10, why) && mentions(why, "region starts there"));
    CHECK(!m.add(48000, 70000, why) && mentions(why, "warp marker at 48000"));
    CHECK(!m.add(kLength + 1, kLength + 1, why) && mentions(why, "past its end"));
    // Landing after the marker it sits before: time would run backwards.
    CHECK(!m.add(24000, 60000, why) && mentions(why, "backwards"));
    CHECK(!m.add(72000, 50000, why) && mentions(why, "between 60000 and the end"));
    CHECK(!m.add(24000, 0, why) && mentions(why, "backwards"));
    // Past what an elastic render holds.
    CHECK(!m.add(90000, api::kMaxElasticFrames, why) && mentions(why, "holds in memory"));
    CHECK(m.markers() == before);

    CHECK(!m.move(0, 0, why) && mentions(why, "backwards"));
    CHECK(!m.move(3, 100, why) && mentions(why, "no warp marker 4"));
    CHECK(!m.remove(1, why) && mentions(why, "no warp marker 2"));
    CHECK(m.markers() == before);
}

void moving_and_removing() {
    WarpMarkers m;
    m.reset(7, kLength);
    std::string why;
    CHECK(m.add(24000, 24000, why) && m.add(48000, 60000, why) && m.add(72000, 84000, why));
    // Between its neighbours' targets it moves; its source stays.
    CHECK(m.move(1, 40000, why));
    CHECK((m.markers()[1] == api::WarpMarker{48000, 40000}));
    CHECK(!m.move(1, 84000, why) && mentions(why, "between 24000 and 84000"));
    CHECK(m.move(2, 72000, why));
    CHECK(m.output_length() == kLength);
    CHECK(m.remove(0, why));
    CHECK((m.markers() == Markers{{48000, 40000}, {72000, 72000}}));
    m.clear();
    CHECK(m.empty() && m.region() == 7);
}

void new_region_drops_markers() {
    WarpMarkers m;
    m.reset(7, kLength);
    std::string why;
    CHECK(m.add(24000, 30000, why));
    m.reset(8, 48000);
    CHECK(m.empty() && m.region() == 8 && m.output_length() == 48000);
    // The old region's frame is past the new one's end.
    CHECK(!m.add(72000, 72000, why) && mentions(why, "past its end"));
}

void cap_on_markers() {
    WarpMarkers m;
    m.reset(7, api::kMaxElasticFrames);
    std::string why;
    for (std::size_t i = 1; i <= api::kMaxElasticMarkers; ++i) {
        const auto f = static_cast<std::int64_t>(i) * 100;
        CHECK(m.add(f, f, why));
    }
    CHECK(!m.add(500000, 500000, why) && mentions(why, "at most 1024"));
    CHECK(m.markers().size() == api::kMaxElasticMarkers);
}

void words_and_rows() {
    WarpMarkers m;
    m.reset(7, kLength);
    std::string why, words;
    CHECK(!m.words(api::ElasticAlgorithm::Rhythmic, words, why) && mentions(why, "moves nothing"));
    CHECK(m.add(48000, 60000, why) && m.add(24000, 24000, why));
    CHECK(m.words(api::ElasticAlgorithm::Rhythmic, words, why));
    CHECK(words == "elastic warp 24000:24000,48000:60000 rhythmic");
    // The words' list is the contract's warp= list, read back equal.
    api::ElasticRequest r;
    CHECK(api::parse_elastic_request("7 rhythmic warp=24000:24000,48000:60000", r, why));
    CHECK(r.markers == m.markers());

    const std::vector<WarpMarkerRow> rows = m.rows(48000);
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        CHECK(rows[0].from == "0.500 s" && rows[0].to == "0.500 s" && rows[0].speed == "x1.00");
        CHECK(rows[1].from == "1.000 s" && rows[1].to == "1.250 s" && rows[1].speed == "x1.50");
    }
    CHECK(m.add(48001, 60007, why));
    CHECK(m.rows(48000)[2].from == "1.000 s");  // a frame past rounds to the millisecond
}

}  // namespace

int main() {
    placing_keeps_order();
    refused_edits_change_nothing();
    moving_and_removing();
    new_region_drops_markers();
    cap_on_markers();
    words_and_rows();
    if (failures == 0) std::printf("ui_elastic_markers: all passed\n");
    return failures == 0 ? 0 : 1;
}
