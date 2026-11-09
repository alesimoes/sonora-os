// SPDX-License-Identifier: GPL-3.0-or-later
//
// The elastic words of tempo.edit (ADR-0136) against the fake engine: each
// word reaches the engine as the request the operator meant, every refusal
// says why and sends nothing, an engine without the stretcher is refused up
// front, and the tempo.edit entry, which cannot reach the engine, refuses
// instead of appearing to stretch.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "edit_controller.h"
#include "model_access.h"
#include "sonora/api/elastic.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/op_codec.h"
#include "sonora/ui_elastic/elastic_command.h"
#include "sonora/ui_elastic/warp_markers.h"
#include "sonora/ui_elastic/warp_panel.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using sonora::api::testing::FakeEngine;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

constexpr model::Frames kTake = 48000;     // the stretched region: one second
constexpr model::Frames kNextAt = 100000;  // where the region after it starts

bool mentions(const std::string& text, const char* part) {
    const bool found = text.find(part) != std::string::npos;
    if (!found) std::fprintf(stderr, "  \"%s\" does not mention \"%s\"\n", text.c_str(), part);
    return found;
}

// A fake engine holding two regions on the first track, `first` at 0 and
// another at kNextAt; the editor works on a copy of its document, so the ids
// are the engine's.
struct Rig {
    explicit Rig(bool capable) {
        api::testing::FakeEngine::Config cfg;
        cfg.tracks = 2;
        if (capable) cfg.capabilities.push_back(api::kCapabilityElastic);
        engine = std::make_unique<FakeEngine>(cfg);
        std::string error;
        CHECK(engine->connect(error));
        engine->poll(in);
        first = place(0, kTake, "Verse vox");
        place(kNextAt, kTake, "Chorus vox");
        doc = std::make_unique<ModelDocument>(engine->document());
        ec = std::make_unique<EditController>(*doc);
        ec->set_mode(EditMode::Slip);
        deps.capabilities = &engine->hello().capabilities;
        deps.send = [this](const std::string& command, const std::string& argument) {
            api::CommandRequest r;
            r.command = command;
            r.argument = argument;
            replies.push_back(reply_to(engine->send(r)));
        };
    }
    api::CommandReply reply_to(uint64_t seq) {
        in.clear();
        engine->poll(in);
        for (const api::CommandReply& c : in.replies) {
            if (c.seq == seq) return c;
        }
        return api::CommandReply{seq, DispatchResult::UnknownCommand, "no reply"};
    }
    model::Id place(model::Frames at, model::Frames frames, const std::string& name) {
        const model::Snapshot& s = engine->document();
        const model::TrackEdit& t = *s.edit->tracks[0];
        model::Id next = s.next_id;
        model::AddSource add;
        add.source.id = next++;
        add.source.path = "Audio Files/" + name + ".wav";
        add.source.channels = t.channels;
        add.source.length = frames;
        model::PlaceRegion p;
        p.playlist = t.active_playlist;
        p.region.id = next++;
        p.region.name = name;
        p.region.position = at;
        p.region.length = frames;
        for (uint16_t c = 0; c < t.channels; ++c) p.region.channels.push_back({add.source.id, c});
        p.split_right_id = next++;
        model::Compound both;
        both.ops = {add, p};
        api::ModelRequest m;
        m.action = api::ModelAction::Apply;
        std::string error;
        CHECK(model::encode_operation(model::Operation{both}, m.op, error));
        CHECK(reply_to(engine->submit(m)).result == DispatchResult::Ok);
        return p.region.id;
    }
    // The words as tempo.edit gets them, with the engine reachable.
    bool run(const std::string& words, std::string& why) {
        why.clear();
        return elastic_command(*ec, deps, words, why);
    }
    const std::vector<api::ElasticRequest>& log() const { return engine->elastic_log(); }

    std::unique_ptr<FakeEngine> engine;
    api::Inbox in;
    model::Id first = 0;
    std::unique_ptr<ModelDocument> doc;
    std::unique_ptr<EditController> ec;
    ElasticDeps deps;
    std::vector<api::CommandReply> replies;
};

void each_word_reaches_the_engine() {
    Rig rig(true);
    CHECK(rig.ec->select_region(rig.first));
    std::string why;

    CHECK(rig.run("elastic ratio 1.5", why));
    CHECK(rig.run("ratio 0.5 rhythmic", why));
    CHECK(rig.run("elastic quantize 1/16 80 monophonic", why));
    CHECK(rig.run("elastic quantize 0.5", why));
    CHECK(rig.run("elastic quantize 1/8 rhythmic", why));
    CHECK(rig.run("elastic quantize 1/4 40% varispeed", why));
    CHECK(rig.run("elastic   warp 1000:1500,24000:26000   varispeed", why));
    if (!why.empty()) std::fprintf(stderr, "  refused: %s\n", why.c_str());
    for (const api::CommandReply& r : rig.replies) CHECK(r.result == DispatchResult::Ok);

    const auto& log = rig.log();
    CHECK(log.size() == 7);
    if (log.size() != 7) return;
    for (const api::ElasticRequest& r : log) CHECK(r.region == rig.first);
    CHECK(log[0].mode == api::ElasticMode::Ratio && log[0].ratio == 1.5);
    CHECK(log[0].algorithm == api::ElasticAlgorithm::Polyphonic);  // the default
    CHECK(log[1].ratio == 0.5 && log[1].algorithm == api::ElasticAlgorithm::Rhythmic);
    // A sixteenth at 4/4 is a quarter of a beat.
    CHECK(log[2].mode == api::ElasticMode::Quantize && log[2].grid_beats == 0.25);
    CHECK(log[2].strength == 80 && log[2].algorithm == api::ElasticAlgorithm::Monophonic);
    CHECK(log[3].grid_beats == 0.5 && log[3].strength == 100);
    CHECK(log[4].grid_beats == 0.5 && log[4].strength == 100 &&
          log[4].algorithm == api::ElasticAlgorithm::Rhythmic);
    CHECK(log[5].grid_beats == 1.0 && log[5].strength == 40 &&
          log[5].algorithm == api::ElasticAlgorithm::Varispeed);
    CHECK(log[6].mode == api::ElasticMode::Warp);
    CHECK((log[6].markers == std::vector<api::WarpMarker>{{1000, 1500}, {24000, 26000}}));
    CHECK(log[6].algorithm == api::ElasticAlgorithm::Varispeed);

    // The editor changes nothing itself: the engine renders and repoints.
    CHECK(find_region(rig.doc->snapshot(), rig.first)->length == kTake);
}

// Refused with the reason, and nothing reaches the engine.
void refusals_send_nothing() {
    Rig rig(true);
    std::string why;
    CHECK(!rig.run("elastic ratio 2", why) && mentions(why, "select a region first"));

    CHECK(rig.ec->select_region(rig.first));
    CHECK(!rig.run("elastic", why) && mentions(why, "elastic ratio"));
    CHECK(!rig.run("elastic stretch 2", why) && mentions(why, "elastic ratio"));
    CHECK(!rig.run("elastic ratio", why) && mentions(why, "elastic ratio"));
    CHECK(!rig.run("elastic ratio 2 granular", why) && mentions(why, "not an elastic algorithm"));
    CHECK(!rig.run("elastic ratio 2 rhythmic extra", why) && mentions(why, "elastic ratio"));
    // The contract's own ranges and words (api/elastic.h).
    CHECK(!rig.run("elastic ratio 9", why) && mentions(why, "outside 0.25..4"));
    CHECK(!rig.run("elastic ratio fast", why) && mentions(why, "is not a number"));
    CHECK(!rig.run("elastic quantize 1/16 0", why) && !why.empty());
    CHECK(!rig.run("elastic quantize 1/0", why) && mentions(why, "is a note (1/16)"));
    CHECK(!rig.run("elastic quantize 99", why) && !why.empty());
    CHECK(!rig.run("elastic warp 2000:3000,1000:4000", why) && !why.empty());
    CHECK(!rig.run("elastic warp 48001:48001", why) && mentions(why, "past the region's end"));
    // Stretched to 1.5 s the first region would run into the second, which
    // starts at kNextAt: refused by name, never rippled.
    CHECK(!rig.run("elastic ratio 3", why) && mentions(why, "frames into \"Chorus vox\"") &&
          mentions(why, "Nothing was stretched"));
    CHECK(!rig.run("elastic warp 40000:96000", why) && mentions(why, "into \"Chorus vox\""));
    // Exactly up to the next region is no overlap.
    std::string ok_why;
    CHECK(rig.run("elastic warp 40000:" + std::to_string(kNextAt - (kTake - 40000)), ok_why));

    CHECK(rig.log().size() == 1);
    CHECK(rig.replies.size() == 1);
}

void capability_absent() {
    Rig rig(false);
    CHECK(rig.ec->select_region(rig.first));
    std::string why;
    // Refused before the words are read: the engine cannot run any of them.
    CHECK(!rig.run("elastic ratio 2", why) && mentions(why, "not available"));
    CHECK(!rig.run("elastic nonsense", why) && mentions(why, "not available"));
    CHECK(!elastic_available(ElasticDeps{}, why) && mentions(why, "without the time stretcher"));
    CHECK(rig.replies.empty());

    // Announced, but no link to send on.
    Rig unlinked(true);
    unlinked.deps.send = nullptr;
    CHECK(unlinked.ec->select_region(unlinked.first));
    CHECK(!unlinked.run("elastic ratio 2", why) && mentions(why, "not connected"));
    CHECK(unlinked.log().empty());
}

// tempo.edit has no route to the engine: it reads the words, refuses with
// the command the operator can run, and never says it stretched.
void tempo_edit_entry_refuses() {
    Rig rig(true);
    CHECK(rig.ec->select_region(rig.first));
    std::string why;
    CHECK(!rig.ec->edit_tempo("elastic ratio 2 rhythmic", why));
    CHECK(mentions(why, "cannot reach the engine from tempo.edit") &&
          mentions(why, ("audio.elastic " + std::to_string(rig.first) + " rhythmic ratio=2").c_str()));
    CHECK(!rig.ec->edit_tempo("elastic ratio 9", why) && mentions(why, "outside 0.25..4"));
    CHECK(!elastic_command(*rig.ec, "elastic quantize 1/16", why) &&
          mentions(why, "quantize=0.25:100"));
    CHECK(rig.log().empty() && rig.replies.empty());
}

// The panel's marker model makes words the engine takes as they were placed.
void marker_model_to_engine() {
    Rig rig(true);
    CHECK(rig.ec->select_region(rig.first));
    WarpMarkers m;
    m.reset(rig.first, kTake);
    std::string why, words;
    CHECK(m.add(24000, 30000, why) && m.add(12000, 12000, why));
    CHECK(m.words(api::ElasticAlgorithm::Monophonic, words, why));
    CHECK(rig.run(words, why));
    CHECK(rig.log().size() == 1);
    if (rig.log().size() == 1) {
        CHECK(rig.log()[0].markers == m.markers());
        CHECK(rig.log()[0].algorithm == api::ElasticAlgorithm::Monophonic);
    }
}

void panel_text() {
    const std::string qml = warp_panel_qml();
    CHECK(mentions(qml, "import QtQuick"));
    CHECK(mentions(qml, "\"tempo.edit\""));
    CHECK(mentions(qml, "warpApply") && mentions(qml, "warpUnavailable"));
}

}  // namespace

int main() {
    each_word_reaches_the_engine();
    refusals_send_nothing();
    capability_absent();
    tempo_edit_entry_refuses();
    marker_model_to_engine();
    panel_text();
    if (failures == 0) std::printf("ui_elastic: all passed\n");
    return failures == 0 ? 0 : 1;
}
