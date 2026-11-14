// SPDX-License-Identifier: GPL-3.0-or-later
// The dialog sends `elastic ratio R ALGO` for the selected region and refuses
// an out-of-range target before anything reaches the engine.
#include <cstdio>
#include <memory>

#include "edit_controller.h"
#include "model_access.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/op_codec.h"
#include "sonora/ui_tce/tce_dialog.h"
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
constexpr model::Frames kTake = 48000;

struct Rig {
    Rig() {
        FakeEngine::Config cfg;
        cfg.tracks = 1;
        cfg.capabilities.push_back(api::kCapabilityElastic);
        engine = std::make_unique<FakeEngine>(cfg);
        std::string error;
        CHECK(engine->connect(error));
        engine->poll(in);
        const model::Snapshot& s = engine->document();
        const model::TrackEdit& t = *s.edit->tracks[0];
        model::Id next = s.next_id;
        model::AddSource add;
        add.source.id = next++;
        add.source.path = "Audio Files/a.wav";
        add.source.channels = t.channels;
        add.source.length = kTake;
        model::PlaceRegion p;
        p.playlist = t.active_playlist;
        p.region.id = next++;
        p.region.name = "a";
        p.region.length = kTake;
        for (uint16_t c = 0; c < t.channels; ++c) p.region.channels.push_back({add.source.id, c});
        p.split_right_id = next++;
        region = p.region.id;
        model::Compound both;
        both.ops = {add, p};
        api::ModelRequest m;
        m.action = api::ModelAction::Apply;
        CHECK(model::encode_operation(model::Operation{both}, m.op, error));
        engine->submit(m);
        in.clear();
        engine->poll(in);
        doc = std::make_unique<ModelDocument>(engine->document());
        ec = std::make_unique<EditController>(*doc);
        ec->set_mode(EditMode::Slip);
        CHECK(ec->select_region(region));
        deps.capabilities = &engine->hello().capabilities;
        deps.send = [this](const std::string& c, const std::string& a) {
            api::CommandRequest r;
            r.command = c;
            r.argument = a;
            engine->send(r);
        };
    }
    std::unique_ptr<FakeEngine> engine;
    api::Inbox in;
    model::Id region = 0;
    std::unique_ptr<ModelDocument> doc;
    std::unique_ptr<EditController> ec;
    ElasticDeps deps;
};
}  // namespace

int main() {
    {
        Rig rig;
        tce::TceDialog d(*rig.ec, rig.deps, kTake);
        std::string why;
        CHECK(d.apply(1.5, "rhythmic", why));
        CHECK(rig.engine->elastic_log().size() == 1);
        CHECK(rig.engine->elastic_log()[0].ratio == 1.5);
        CHECK(d.resulting_length() == 72000);
    }
    {
        Rig rig;
        tce::TceDialog d(*rig.ec, rig.deps, kTake);
        std::string why;
        CHECK(!d.apply(5.0, "polyphonic", why) && !why.empty());
        CHECK(!d.apply(0.1, "polyphonic", why));
        CHECK(rig.engine->elastic_log().empty());
        CHECK(d.resulting_length() == 0);
    }
    return failures == 0 ? 0 : 1;
}
