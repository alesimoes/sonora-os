// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.elastic's contract (ADR-0136 §2): the argument round trips, every
// refusal with its reason, the capability gating, and FakeEngine checking a
// request against its own document with the contract's words.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/elastic.h"
#include "sonora/api/fake_engine.h"
#include "sonora/command.h"
#include "sonora/model/op_codec.h"

using namespace sonora::api;
using sonora::DispatchResult;
using sonora::api::testing::FakeEngine;
namespace model = sonora::model;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

bool mentions(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

ElasticRequest parsed(const std::string& text) {
    ElasticRequest r;
    std::string why;
    CHECK(parse_elastic_request(text, r, why));
    if (!why.empty()) std::fprintf(stderr, "  %s: %s\n", text.c_str(), why.c_str());
    return r;
}

void round_trips() {
    // Canonical texts come back as they went.
    for (const char* text : {"7 polyphonic ratio=1.5", "7 rhythmic ratio=0.25", "7 monophonic ratio=4",
                             "12 varispeed ratio=1.000001", "3 rhythmic warp=48000:52000",
                             "3 polyphonic warp=100:90,48000:52000,96000:96000",
                             "9 monophonic quantize=0.25:100", "9 rhythmic quantize=1:40",
                             "9 varispeed quantize=0.0625:1", "9 polyphonic quantize=16:100"}) {
        CHECK(format_elastic_request(parsed(text)) == text);
    }

    ElasticRequest r = parsed("  42   rhythmic   warp=10:20,30:35  ");
    CHECK(r.region == 42 && r.algorithm == ElasticAlgorithm::Rhythmic);
    CHECK(r.mode == ElasticMode::Warp && r.markers.size() == 2);
    CHECK((r.markers[1] == WarpMarker{30, 35}));

    r = parsed("5 polyphonic ratio=2.125");
    CHECK(r.mode == ElasticMode::Ratio && r.ratio == 2.125);

    // Strength defaults to 100 and is then written out.
    r = parsed("5 monophonic quantize=0.5");
    CHECK(r.mode == ElasticMode::Quantize && r.grid_beats == 0.5 && r.strength == 100);
    CHECK(format_elastic_request(r) == "5 monophonic quantize=0.5:100");

    // A value built in code is written to six places and read back equal.
    ElasticRequest built;
    built.region = 8;
    built.algorithm = ElasticAlgorithm::Varispeed;
    built.ratio = 1.25;
    CHECK(parsed(format_elastic_request(built)) == built);
    built.mode = ElasticMode::Quantize;
    built.grid_beats = 0.125;
    built.strength = 75;
    CHECK(parsed(format_elastic_request(built)) == built);
    built.mode = ElasticMode::Warp;
    built.markers = {{1, 2}, {3, 9}};
    CHECK(parsed(format_elastic_request(built)) == built);
}

void refused(const std::string& text, const char* reason) {
    ElasticRequest r;
    r.region = 99;
    const ElasticRequest before = r;
    std::string why;
    const bool ok = parse_elastic_request(text, r, why);
    CHECK(!ok);
    CHECK(r == before);   // nothing half-parsed is left behind
    if (ok || !mentions(why, reason)) {
        std::fprintf(stderr, "FAIL \"%s\": got \"%s\", wanted \"%s\"\n", text.c_str(), why.c_str(),
                     reason);
        ++failures;
    }
}

void parse_refusals() {
    refused("", "needs REGION ALGORITHM");
    refused("7 polyphonic", "needs REGION ALGORITHM");
    refused("7 polyphonic ratio=2 ratio=2", "needs REGION ALGORITHM");
    refused("0 polyphonic ratio=2", "\"0\" is no region id");
    refused("-3 polyphonic ratio=2", "is no region id");
    refused("x polyphonic ratio=2", "is no region id");
    refused("7 granular ratio=2", "unknown algorithm \"granular\"");
    refused("7 Polyphonic ratio=2", "unknown algorithm");
    refused("7 polyphonic ratio", "is not ratio=, warp= or quantize= with a value");
    refused("7 polyphonic ratio=", "is not ratio=, warp= or quantize= with a value");
    refused("7 polyphonic speed=2", "unknown mode \"speed\"");
    refused("7 polyphonic ratio=abc", "ratio \"abc\" is not a number");
    refused("7 polyphonic ratio=1,5", "is not a number");
    refused("7 polyphonic ratio=.5", "is not a number");
    refused("7 polyphonic ratio=1.", "is not a number");
    refused("7 polyphonic ratio=1.0000001", "is not a number");
    refused("7 polyphonic ratio=-2", "is not a number");
    refused("7 polyphonic ratio=0.2", "outside 0.25..4");
    refused("7 polyphonic ratio=4.000001", "outside 0.25..4");
    refused("7 polyphonic warp=100", "\"100\" is not SRC:DST in frames");
    refused("7 polyphonic warp=100:200,", "\"\" is not SRC:DST");
    refused("7 polyphonic warp=a:1", "is not SRC:DST");
    refused("7 polyphonic warp=1:-1", "is not SRC:DST");
    refused("7 polyphonic warp=0:0", "must move forward on both sides");
    refused("7 polyphonic warp=100:0", "must move forward on both sides");
    refused("7 polyphonic warp=100:200,100:300", "must move forward on both sides");
    refused("7 polyphonic warp=100:200,150:200", "must move forward on both sides");
    refused("7 polyphonic warp=200:200,100:300", "must move forward on both sides");
    std::string many;
    for (std::size_t i = 1; i <= kMaxElasticMarkers + 1; ++i) {
        if (i > 1) many += ",";
        many += std::to_string(i) + ":" + std::to_string(i);
    }
    refused("7 polyphonic warp=" + many, "more than 1024 warp markers");
    refused("7 polyphonic quantize=x", "grid \"x\" is not a number of beats");
    refused("7 polyphonic quantize=0.03125", "outside 0.0625..16");
    refused("7 polyphonic quantize=17", "outside 0.0625..16");
    refused("7 polyphonic quantize=0.25:", "strength \"\" is not a percentage");
    refused("7 polyphonic quantize=0.25:x", "is not a percentage");
    refused("7 polyphonic quantize=0.25:0", "outside 1..100");
    refused("7 polyphonic quantize=0.25:101", "outside 1..100");
}

void validation() {
    std::string why;
    ElasticRequest r = parsed("4 polyphonic ratio=2");
    CHECK(validate_elastic_request(r, 48000, why));
    CHECK(elastic_output_frames(r, 48000) == 96000);
    CHECK(!validate_elastic_request(r, 0, why) && mentions(why, "is empty"));
    CHECK(!validate_elastic_request(r, kMaxElasticFrames + 1, why) &&
          mentions(why, "holds in memory; split it first"));
    // The source fits, the result does not.
    CHECK(!validate_elastic_request(r, kMaxElasticFrames / 2 + 1, why) &&
          mentions(why, "the stretched region would be"));
    CHECK(validate_elastic_request(r, kMaxElasticFrames / 2, why));
    // A ratio set in code is held to the same range.
    r.ratio = 5.0;
    CHECK(!validate_elastic_request(r, 48000, why) && mentions(why, "outside 0.25..4"));

    r = parsed("4 rhythmic warp=1000:2000,48000:47000");
    CHECK(validate_elastic_request(r, 48000, why));
    CHECK(elastic_output_frames(r, 48000) == 47000);
    CHECK(validate_elastic_request(r, 50000, why));
    CHECK(elastic_output_frames(r, 50000) == 49000);   // the tail keeps its speed
    CHECK(!validate_elastic_request(r, 47999, why) && mentions(why, "past the region's end"));
    r.markers.clear();
    CHECK(!validate_elastic_request(r, 48000, why) && mentions(why, "at least one marker"));
    r.markers = {{10, 10}, {5, 20}};
    CHECK(!validate_elastic_request(r, 48000, why) && mentions(why, "move forward"));
    r.markers = {{10, kMaxElasticFrames}};
    CHECK(!validate_elastic_request(r, 48000, why) && mentions(why, "the stretched region"));

    r = parsed("4 monophonic quantize=0.25:50");
    CHECK(validate_elastic_request(r, 48000, why));
    CHECK(elastic_output_frames(r, 48000) == -1);
}

void catalog_entry() {
    const sonora::CommandSpec* spec = sonora::find_command("audio.elastic");
    CHECK(spec != nullptr);
    if (spec == nullptr) return;
    CHECK(spec->takes_argument);
    CHECK(spec->domain == sonora::CommandDomain::Engine);
    CHECK(spec->target == sonora::CommandTarget::None);
    CHECK(std::string(kCapabilityElastic) == "audio.elastic");
}

struct Rig {
    explicit Rig(bool capable) {
        FakeEngine::Config cfg;
        cfg.tracks = 2;
        if (capable) cfg.capabilities.push_back(kCapabilityElastic);
        engine = std::make_unique<FakeEngine>(cfg);
        std::string error;
        CHECK(engine->connect(error));
        engine->poll(in);
    }
    CommandReply reply_to(uint64_t seq) {
        in.clear();
        engine->poll(in);
        for (const CommandReply& c : in.replies) {
            if (c.seq == seq) return c;
        }
        return CommandReply{seq, DispatchResult::UnknownCommand, "no reply"};
    }
    CommandReply run(const std::string& arg) {
        CommandRequest r;
        r.command = "audio.elastic";
        r.argument = arg;
        return reply_to(engine->send(r));
    }
    CommandReply apply(const model::Operation& op) {
        ModelRequest m;
        m.action = ModelAction::Apply;
        std::string error;
        CHECK(model::encode_operation(op, m.op, error));
        return reply_to(engine->submit(m));
    }
    // One region of `frames` on the first track; its id.
    model::Id place_region(model::Frames frames) {
        const model::Snapshot& doc = engine->document();
        const model::TrackEdit& t = *doc.edit->tracks[0];
        model::Id next = doc.next_id;
        model::AddSource add;
        add.source.id = next++;
        add.source.path = "Audio Files/take.wav";
        add.source.channels = t.channels;
        add.source.length = frames;
        model::PlaceRegion p;
        p.playlist = t.active_playlist;
        p.region.id = next++;
        p.region.name = "take";
        p.region.length = frames;
        for (uint16_t c = 0; c < t.channels; ++c) p.region.channels.push_back({add.source.id, c});
        p.split_right_id = next++;
        model::Compound both;
        both.ops = {add, p};
        CHECK(apply(both).result == DispatchResult::Ok);
        return p.region.id;
    }
    std::unique_ptr<FakeEngine> engine;
    Inbox in;
};

bool has(const std::vector<std::string>& list, const char* id) {
    for (const std::string& s : list) {
        if (s == id) return true;
    }
    return false;
}

void capability_gating() {
    Rig plain(false);
    CHECK(!has(plain.engine->hello().capabilities, kCapabilityElastic));
    CHECK(!has(plain.engine->hello().implemented_commands, "audio.elastic"));
    const model::Id region = plain.place_region(48000);
    const CommandReply r = plain.run(std::to_string(region) + " polyphonic ratio=2");
    CHECK(r.result == DispatchResult::NotImplemented);
    CHECK(plain.engine->elastic_log().empty());

    Rig capable(true);
    CHECK(has(capable.engine->hello().capabilities, kCapabilityElastic));
    CHECK(has(capable.engine->hello().implemented_commands, "audio.elastic"));
}

void fake_engine() {
    Rig rig(true);
    const model::Id region = rig.place_region(48000);
    const std::string id = std::to_string(region);

    const std::string ratio = id + " polyphonic ratio=1.5";
    CHECK(rig.run(ratio).result == DispatchResult::Ok);
    const std::string warp = id + " rhythmic warp=1000:1500,48000:52000";
    CHECK(rig.run(warp).result == DispatchResult::Ok);
    const std::string quantize = id + " monophonic quantize=0.25:80";
    CHECK(rig.run(quantize).result == DispatchResult::Ok);
    const auto& log = rig.engine->elastic_log();
    CHECK(log.size() == 3);
    if (log.size() == 3) {
        CHECK(log[0] == parsed(ratio));
        CHECK(log[1] == parsed(warp));
        CHECK(log[2] == parsed(quantize));
    }

    // Refused with the contract's own words, and not recorded.
    const auto same_refusal = [&](const std::string& text, int64_t region_frames) {
        ElasticRequest r;
        std::string why;
        if (parse_elastic_request(text, r, why)) {
            CHECK(!validate_elastic_request(r, region_frames, why));
        }
        const CommandReply c = rig.run(text);
        CHECK(c.result == DispatchResult::Failed);
        CHECK(!why.empty() && c.message == why);
        if (c.message != why) {
            std::fprintf(stderr, "  fake \"%s\" vs contract \"%s\"\n", c.message.c_str(),
                         why.c_str());
        }
    };
    same_refusal(id + " granular ratio=2", 48000);
    same_refusal(id + " polyphonic ratio=9", 48000);
    same_refusal(id + " polyphonic warp=100:200,50:300", 48000);
    same_refusal(id + " polyphonic warp=48001:48001", 48000);
    same_refusal(id + " polyphonic quantize=0.25:0", 48000);
    same_refusal("7 polyphonic", 48000);

    const CommandReply missing = rig.run(std::to_string(region + 1000) + " polyphonic ratio=2");
    CHECK(missing.result == DispatchResult::Failed &&
          mentions(missing.message, "is not an audio region of this session"));
    CHECK(rig.engine->elastic_log().size() == 3);

    // A region too long to hold is refused by its length.
    Rig big(true);
    const model::Id huge = big.place_region(kMaxElasticFrames + 1);
    const CommandReply over = big.run(std::to_string(huge) + " varispeed ratio=1");
    CHECK(over.result == DispatchResult::Failed &&
          mentions(over.message, "holds in memory; split it first"));
    CHECK(big.engine->elastic_log().empty());
}

}  // namespace

int main() {
    round_trips();
    parse_refusals();
    validation();
    catalog_entry();
    capability_gating();
    fake_engine();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("api_elastic: ok\n");
    return 0;
}
