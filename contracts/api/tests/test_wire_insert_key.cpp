// SPDX-License-Identifier: GPL-3.0-or-later
//
// Side-chain keys on the wire (ADR-0058, protocol 69): SetInsertKey crosses
// inside a ModelRequest and decodes to what was sent, and the catalog says
// which plug-ins have a key input (PluginInfo::key_inputs).
#include <cstdio>
#include <string>

#include "sonora/api/messages.h"
#include "sonora/api/wire.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/operations.h"

using namespace sonora;
using namespace sonora::api;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

void set_insert_key_crosses() {
    ModelRequest m;
    m.seq = 7;
    m.action = ModelAction::Apply;
    std::string error;
    CHECK(model::encode_operation(model::SetInsertKey{12, 34, 56}, m.op, error));
    std::string bytes;
    wire::encode(m, bytes);
    ModelRequest back;
    CHECK(wire::decode(bytes, back, error));
    CHECK(back.seq == 7 && back.action == ModelAction::Apply && back.op == m.op);
    model::Operation op;
    CHECK(model::decode_operation(back.op, op, error));
    const auto* k = op.get_if<model::SetInsertKey>();
    CHECK(k != nullptr && k->track == 12 && k->insert == 34 && k->key_bus == 56);
    CHECK(std::string(model::operation_name(op)) == "SetInsertKey");
    CHECK(model::is_undoable(op));

    // A key the codec does not define is refused, not dropped.
    std::string extra = back.op;
    extra.insert(extra.rfind('}'), ",\"side\":1");
    CHECK(!model::decode_operation(extra, op, error));
}

void catalog_carries_key_inputs() {
    PluginCatalog c;
    c.generation = 3;
    c.plugins.resize(2);
    c.plugins[0].id = "urn:sonora:gate";
    c.plugins[0].name = "Gate";
    c.plugins[0].inputs = 2;
    c.plugins[0].outputs = 2;
    c.plugins[0].key_inputs = 2;
    c.plugins[1].id = "urn:sonora:eq";
    c.plugins[1].name = "EQ";
    c.plugins[1].inputs = 1;
    c.plugins[1].outputs = 1;
    std::string bytes, error;
    CHECK(wire::encode(c, bytes, error));
    PluginCatalog back;
    CHECK(wire::decode(bytes, back, error));
    CHECK(back.plugins.size() == 2);
    CHECK(back.plugins[0].key_inputs == 2 && back.plugins[0].inputs == 2);
    CHECK(back.plugins[1].key_inputs == 0 && back.plugins[1].outputs == 1);
    // Every field is read: one byte short is refused, never half a catalog.
    bytes.pop_back();
    CHECK(!wire::decode(bytes, back, error));
}

}  // namespace

int main() {
    CHECK(kProtocolVersion == 69);
    set_insert_key_crosses();
    catalog_carries_key_inputs();
    if (failures != 0) {
        std::fprintf(stderr, "api_wire_insert_key: %d failed\n", failures);
        return 1;
    }
    std::puts("api_wire_insert_key: ok");
    return 0;
}
