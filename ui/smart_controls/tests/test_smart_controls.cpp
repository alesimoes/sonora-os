#include <cmath>
#include <cstdio>
#include <string>

#include "sonora/smart_controls/smart_controls.h"

using namespace sonora::smart_controls;

static int failures = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);      \
            ++failures;                                           \
        }                                                         \
    } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static SmartControlSet sample() {
    SmartControlSet s;
    std::string why;
    Macro m{"Tone",
            {{1, "cutoff", 0.2f, 0.8f, false},
             {1, "res", 0.0f, 1.0f, true},
             {2, "mix", 0.0f, 0.5f, false}}};
    CHECK(add_macro(s, m, why));
    return s;
}

int main() {
    auto s = sample();
    FakeSink sink;
    sink.known = {{1, "cutoff"}, {1, "res"}, {2, "mix"}};
    ApplyResult r;
    std::string why;
    CHECK(apply(s, 0, 0.25f, sink, r, why));
    CHECK(r.applied == 3 && r.skipped == 0);
    CHECK(sink.calls.size() == 7);  // two inserts: b s s e b s e
    CHECK(sink.calls[0].kind == 'b' && sink.calls[3].kind == 'e' && sink.calls[4].kind == 'b');
    CHECK(near(sink.calls[1].value, 0.35f));  // lo/hi
    CHECK(near(sink.calls[2].value, 0.75f));  // invert
    CHECK(near(sink.calls[5].value, 0.125f));
    CHECK(!apply(s, 3, 0.5f, sink, r, why) && !why.empty());
    CHECK(!apply(s, 0, 1.5f, sink, r, why));

    FakeSink partial;
    partial.known = {{2, "mix"}};
    ApplyResult r2;
    CHECK(apply(s, 0, 1.0f, partial, r2, why));
    CHECK(r2.applied == 1 && r2.skipped == 2 && partial.calls.size() == 3);
    CHECK(s.macros[0].mappings.size() == 3);

    SmartControlSet big;
    for (int i = 0; i < 8; ++i) CHECK(add_macro(big, {"m" + std::to_string(i), {}}, why));
    CHECK(!add_macro(big, {"m9", {}}, why) && big.macros.size() == 8);
    CHECK(!add_macro(s, {"Tone", {}}, why) && s.macros.size() == 1);
    CHECK(!add_macro(s, {"Dup", {{1, "a", 0, 1, false}, {1, "a", 0, 1, true}}}, why));
    CHECK(!add_macro(s, {"", {}}, why));

    MemoryStore ms;
    CHECK(ms.save(s, why));
    SmartControlSet back;
    CHECK(ms.load(back, why));
    CHECK(back.macros.size() == 1 && back.macros[0].mappings.size() == 3);
    CHECK(back.macros[0].mappings[1].invert && near(back.macros[0].mappings[0].lo, 0.2f));
    CHECK(to_json(back) == to_json(s));

    ms.text = "{not json";
    CHECK(!ms.load(back, why) && why.find("corrupt") != std::string::npos);
    ms.text = "{\"schema\":2,\"macros\":[]}";
    CHECK(!ms.load(back, why) && why.find("schema 2") != std::string::npos);
    CHECK(back.macros.size() == 1);  // untouched on failure

    JsonFileStore fs("smart_controls_test.json");
    CHECK(fs.save(s, why));
    SmartControlSet fb;
    CHECK(fs.load(fb, why) && fb.macros.size() == 1);
    std::remove("smart_controls_test.json");
    CHECK(!fs.load(fb, why));

    std::printf(failures ? "FAILED\n" : "ok\n");
    return failures ? 1 : 0;
}
