// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key chooser's logic (ADR-0058 item 6), on a ModelDocument:
// no key offered to a plugin without a side-chain input, "None" plus every
// aux bus by name for one with it, one undo step to key and to unkey, a
// loop refused with the model's text, and a key whose bus is gone shown as
// missing rather than as none.
#include <cstdio>
#include <memory>
#include <string>

#include "insert_controller.h"
#include "insert_key_controller.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/insert_key.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

constexpr int kGain = 0, kComp = 2;

api::PluginCatalog catalog() {
    api::PluginCatalog c;
    c.plugins = api::testing::fake_catalog();
    return c;
}

struct Rig {
    ModelDocument doc{make_empty_session(2, 48000)};
    InsertController inserts{doc};
    InsertKeyController keys{inserts};
    model::Id verb = 0, delay = 0;

    Rig() {
        inserts.set_catalog(catalog());
        std::string why;
        inserts.add(0, 0, kComp, why);
        inserts.add(0, 1, kGain, why);
        verb = doc.reserve_id();
        doc.submit(model::AddBus{verb, "Verb", model::BusMix{}}, why);
        delay = doc.reserve_id();
        doc.submit(model::AddBus{delay, "Delay", model::BusMix{}}, why);
    }
};

int only_a_keyed_plugin_is_offered() {
    Rig r;
    CHECK(r.keys.view(0, 0).capable);
    CHECK(!r.keys.view(0, 1).capable);
    CHECK(!r.keys.view(0, 4).capable);   // empty slot
    CHECK(r.keys.choices(0, 1).empty());
    std::string why;
    CHECK(!r.keys.set(0, 1, r.verb, why));
    CHECK(why.find("no side-chain input") != std::string::npos);
    return 0;
}

int lists_none_and_every_bus_by_name() {
    Rig r;
    const auto c = r.keys.choices(0, 0);
    CHECK(c.size() == 3);
    CHECK(c[0].name == "None" && c[0].bus == kNoId && c[0].current);
    CHECK(c[1].name == "Verb" && c[2].name == "Delay");
    return 0;
}

int keying_is_one_undo_step() {
    Rig r;
    std::string why;
    CHECK(r.keys.set(0, 0, r.delay, why));
    CHECK(r.keys.view(0, 0).name == "Delay");
    CHECK(r.keys.choices(0, 0)[2].current);
    CHECK(r.doc.undo(why));
    CHECK(r.keys.view(0, 0).bus == kNoId);
    CHECK(r.doc.redo(why));
    CHECK(r.keys.set(0, 0, kNoId, why));
    CHECK(r.keys.view(0, 0).bus == kNoId && !r.keys.view(0, 0).missing);
    return 0;
}

int a_loop_is_refused_with_the_models_text() {
    Rig r;
    std::string why;
    model::AddSend send;
    send.strip = r.inserts.snapshot().edit->tracks[0]->id;
    send.send = r.doc.reserve_id();
    send.slot = 0;
    send.bus = r.verb;
    CHECK(r.doc.submit(send, why));
    CHECK(!r.keys.set(0, 0, r.verb, why));
    CHECK(why.find(model::insert_key_refusal::kLoop) != std::string::npos);
    CHECK(r.keys.view(0, 0).bus == kNoId);
    return 0;
}

int a_stale_bus_reads_as_missing() {
    Rig r;
    std::string why;
    CHECK(r.keys.set(0, 0, r.delay, why));
    // An old file can key a bus that is gone (RemoveBus unkeys, so the state
    // is made directly).
    model::Snapshot s = r.doc.snapshot();
    auto edit = std::make_shared<model::EditState>(*s.edit);
    edit->buses.clear();
    s.edit = edit;
    ModelDocument stale{s};
    InsertController inserts{stale};
    inserts.set_catalog(catalog());
    InsertKeyController keys{inserts};
    const KeyView v = keys.view(0, 0);
    CHECK(v.capable && v.missing && v.name.empty() && v.bus == r.delay);
    return 0;
}

int the_list_follows_a_bus_rename() {
    Rig r;
    std::string why;
    CHECK(r.keys.set(0, 0, r.delay, why));
    model::SetBusPath rename;
    rename.bus = r.delay;
    rename.name = "Echo";
    CHECK(r.doc.submit(rename, why));
    // Read afresh: the same names must not be cached from before the rename.
    CHECK(r.keys.choices(0, 0)[2].name == "Echo");
    CHECK(r.keys.choices(0, 0)[2].current);
    CHECK(r.keys.view(0, 0).name == "Echo");
    // The key is one undo step: the first undo reverts the rename and keeps
    // the key, the second removes the key.
    CHECK(r.doc.undo(why));
    CHECK(r.keys.view(0, 0).name == "Delay" && r.keys.view(0, 0).bus == r.delay);
    CHECK(r.doc.undo(why));
    CHECK(r.keys.view(0, 0).bus == kNoId);
    return 0;
}

}  // namespace

int main() {
    if (the_list_follows_a_bus_rename() != 0) return 1;
    if (only_a_keyed_plugin_is_offered() != 0) return 1;
    if (lists_none_and_every_bus_by_name() != 0) return 1;
    if (keying_is_one_undo_step() != 0) return 1;
    if (a_loop_is_refused_with_the_models_text() != 0) return 1;
    if (a_stale_bus_reads_as_missing() != 0) return 1;
    std::printf("ui insert key: all checks passed\n");
    return 0;
}
