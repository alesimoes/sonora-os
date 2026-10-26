// SPDX-License-Identifier: GPL-3.0-or-later
// The probe record, version 2 (probe_record.h): the golden line is pinned,
// every record round-trips exactly, a version-1 record still reads (as a
// native plugin's), a record of another version is refused as that, and
// malformed, mistyped, inconsistent or over-cap records are refused rather
// than guessed at. A fuzzed line never crashes the decoder.
#include "sonora/plugin/probe_record.h"

#include <cstdio>
#include <random>
#include <string>

using namespace sonora::plugin::probe;

static_assert(kProbeRecordVersion == 2, "the golden lines below describe record version 2");

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

Record sample() {
    Record r;
    r.host = "sonora-plugin-host layout 1 clock 1 wire 1 probe 2";
    r.uri = "urn:sonora:test#gain";
    r.format = "lv2";
    r.bundle = "/usr/lib/lv2/sonora-test.lv2";
    r.name = "Gain \"test\"";
    r.vendor = "Sonora";
    r.version = "1.2";
    r.categories = {"Utility", "Amplifier"};
    r.audio_in = 1;
    r.audio_out = 1;
    r.outcome = Outcome::Ok;
    r.channels = 2;
    r.replicas = 2;
    r.latency_frames = 0;
    r.sample_rate = 48000;
    Param g;
    g.key = "gain";
    g.symbol = "gain";
    g.name = "Gain";
    g.min = -90.f;
    g.max = 24.f;
    g.default_value = 0.f;
    g.flags = kParamAutomatable;
    g.unit = "dB";
    Param m;
    m.key = "mode";
    m.symbol = "mode";
    m.name = "Mode\tA";
    m.min = 0.f;
    m.max = 2.f;
    m.default_value = 1.f;
    m.flags = kParamAutomatable | kParamEnum;
    m.steps = 3;
    m.labels = {"Off", "Peak", "RMS é"};
    r.params = {g, m};
    return r;
}

bool same(const Record& a, const Record& b) {
    if (a.params.size() != b.params.size()) return false;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        const Param& p = a.params[i];
        const Param& q = b.params[i];
        if (p.key != q.key || p.symbol != q.symbol || p.name != q.name || p.min != q.min || p.max != q.max ||
            p.default_value != q.default_value || p.flags != q.flags || p.unit != q.unit || p.steps != q.steps ||
            p.labels != q.labels)
            return false;
    }
    return a.record == b.record && a.host == b.host && a.uri == b.uri && a.format == b.format &&
           a.bundle == b.bundle && a.bridge == b.bridge && a.name == b.name && a.vendor == b.vendor && a.version == b.version &&
           a.categories == b.categories && a.has_editor == b.has_editor && a.audio_in == b.audio_in &&
           a.audio_out == b.audio_out && a.sidechain_in == b.sidechain_in && a.outcome == b.outcome &&
           a.reason == b.reason && a.channels == b.channels && a.replicas == b.replicas &&
           a.latency_frames == b.latency_frames && a.sample_rate == b.sample_rate &&
           a.plugin_bypass == b.plugin_bypass;
}

void golden() {
    const std::string want =
        "{\"record\":2,\"host\":\"sonora-plugin-host layout 1 clock 1 wire 1 probe 2\",\"uri\":\"urn:sonora:test#gain\","
        "\"format\":\"lv2\",\"bundle\":\"/usr/lib/lv2/sonora-test.lv2\",\"name\":\"Gain \\\"test\\\"\","
        "\"vendor\":\"Sonora\",\"version\":\"1.2\",\"categories\":[\"Utility\",\"Amplifier\"],\"has_editor\":false,"
        "\"audio_in\":1,\"audio_out\":1,\"sidechain_in\":0,\"ok\":true,\"channels\":2,\"replicas\":2,"
        "\"latency_frames\":0,\"sample_rate\":48000,\"outcome\":\"ok\",\"bypass\":\"host\",\"params\":["
        "{\"key\":\"gain\",\"symbol\":\"gain\",\"name\":\"Gain\",\"min\":-90.0,\"max\":24.0,\"default\":0.0,"
        "\"flags\":1,\"unit\":\"dB\",\"steps\":0,\"labels\":[]},"
        "{\"key\":\"mode\",\"symbol\":\"mode\",\"name\":\"Mode\\tA\",\"min\":0.0,\"max\":2.0,\"default\":1.0,"
        "\"flags\":5,\"unit\":\"\",\"steps\":3,\"labels\":[\"Off\",\"Peak\",\"RMS é\"]}]}";
    const std::string got = encode(sample());
    if (got != want) std::printf("golden:\n  got  %s\n  want %s\n", got.c_str(), want.c_str());
    CHECK(got == want);
    // A bridged VST3 (ADR-0017 Y2): `bridge` right after `bundle`, as the
    // host's --list has it.
    Record r = sample();
    r.uri = "vst3:534F4E4F524156535433544553540009";
    r.format = "vst3";
    r.bundle = "/home/op/.vst3/yabridge/sonora-test-state.vst3";
    r.bridge = "yabridge";
    r.params.clear();
    const std::string bridged = encode(r);
    CHECK(bridged.find("\"bundle\":\"/home/op/.vst3/yabridge/sonora-test-state.vst3\",\"bridge\":\"yabridge\","
                       "\"name\":") != std::string::npos);
}

void round_trips() {
    Record r = sample();
    Record back;
    std::string e;
    CHECK(decode(encode(r), back, e) && same(r, back));
    // A refusal, with its reason, after ok (the host tests grep for it).
    r.outcome = Outcome::Refused;
    r.reason = "wrote outside them";
    const std::string line = encode(r);
    CHECK(line.find("\"ok\":false,\"reason\":\"wrote outside them\"") != std::string::npos);
    CHECK(decode(line + "\n", back, e) && same(r, back));
    r.outcome = Outcome::Blocked;
    r.reason = "its probe was killed by signal 11";
    CHECK(decode(encode(r), back, e) && same(r, back));
    r.outcome = Outcome::NotInstalled;
    r.reason = "not installed";
    r.bundle.clear();
    r.params.clear();
    CHECK(decode(encode(r), back, e) && same(r, back));
    // Floats come back bit for bit, whatever their shape.
    r = sample();
    r.params[0].min = 1e-7f;
    r.params[0].max = 3.4e38f;
    r.params[0].default_value = 0.1f;
    CHECK(decode(encode(r), back, e) && same(r, back));
    // Escapes and \u from another writer.
    const std::string other =
        "{\"record\":1,\"host\":\"h\",\"uri\":\"u\",\"format\":\"vst3\",\"bundle\":\"\\/b\",\"name\":\"\\u00e9\\ud83c\\udfb5\","
        "\"vendor\":\"\",\"version\":\"\",\"categories\":[],\"has_editor\":true,\"audio_in\":2,\"audio_out\":2,"
        "\"sidechain_in\":2,\"ok\":true,\"channels\":2,\"replicas\":1,\"latency_frames\":240,\"sample_rate\":48000,"
        "\"outcome\":\"ok\",\"bypass\":\"plugin\",\"params\":[]}";
    CHECK(decode(other, back, e));
    CHECK(back.bundle == "/b" && back.name == "\xc3\xa9\xf0\x9f\x8e\xb5" && back.plugin_bypass && back.has_editor);
    // That line is a version-1 record, as an engine before ADR-0017's
    // follow-up wrote it: it still reads, as a native plugin's.
    CHECK(back.record == 1 && back.bridge.empty());
    // A bridged plugin's record comes back bridged.
    r = sample();
    r.format = "vst3";
    r.uri = "vst3:534F4E4F524156535433544553540009";
    r.bridge = "yabridge";
    CHECK(decode(encode(r), back, e) && same(r, back) && back.bridge == "yabridge");
    r.outcome = Outcome::Blocked;
    r.reason = "its probe did not answer within 30 s";
    CHECK(decode(encode(r), back, e) && same(r, back));
}

void refusals() {
    Record back;
    std::string e;
    const std::string good = encode(sample());
    const auto refused = [&](std::string line, const char* says) {
        const bool ok = !decode(line, back, e) && e.find(says) != std::string::npos;
        if (!ok) std::printf("  wanted a refusal saying \"%s\", got \"%s\"\n", says, e.c_str());
        return ok;
    };
    const auto swap = [&](const std::string& from, const std::string& to) {
        std::string s = good;
        const auto at = s.find(from);
        if (at != std::string::npos) s.replace(at, from.size(), to);
        return s;
    };
    CHECK(refused(swap("\"record\":2", "\"record\":3"), "version 3"));
    CHECK(refused(swap("\"record\":2", "\"record\":0"), "version 0"));
    CHECK(refused(swap("\"record\":2,", ""), "no \"record\""));
    // `bridge`: version 2's only, "yabridge" only, a VST3's only.
    const std::string bridged_lv2 = swap("\"name\":", "\"bridge\":\"yabridge\",\"name\":");
    CHECK(refused(bridged_lv2, "not a VST3"));
    std::string vst3 = good;
    vst3.replace(vst3.find("\"format\":\"lv2\""), 14, "\"format\":\"vst3\"");
    const auto with_bridge = [&](const std::string& line, const std::string& value) {
        std::string s = line;
        s.replace(s.find("\"name\":"), 7, "\"bridge\":" + value + ",\"name\":");
        return s;
    };
    CHECK(decode(with_bridge(vst3, "\"yabridge\""), back, e) && back.bridge == "yabridge");
    CHECK(refused(with_bridge(vst3, "\"wine\""), "bridge is \"wine\""));
    CHECK(refused(with_bridge(vst3, "\"\""), "bridge is \"\""));
    CHECK(refused(with_bridge(vst3, "1"), "bridge is not a string"));
    std::string v1 = with_bridge(vst3, "\"yabridge\"");
    v1.replace(v1.find("\"record\":2"), 10, "\"record\":1");
    CHECK(refused(v1, "unknown key \"bridge\""));
    CHECK(refused(swap("\"ok\":true", "\"ok\":false"), "disagree"));
    CHECK(refused(swap("\"outcome\":\"ok\"", "\"outcome\":\"maybe\""), "outcome"));
    CHECK(refused(swap("\"format\":\"lv2\"", "\"format\":\"au\""), "format"));
    CHECK(refused(swap("\"steps\":3", "\"steps\":1"), "1 step"));
    CHECK(refused(swap("\"steps\":3", "\"steps\":4"), "3 labels for 4 steps"));
    CHECK(refused(swap("\"flags\":5", "\"flags\":64"), "unknown flags"));
    CHECK(refused(swap("\"vendor\":\"Sonora\"", "\"vendor\":1"), "wrong type"));
    CHECK(refused(swap("\"vendor\"", "\"maker\""), "unknown key"));
    CHECK(refused(swap("\"audio_in\":1", "\"audio_in\":-1"), "32-bit"));
    CHECK(refused(swap("\"min\":-90.0", "\"min\":1e999"), "finite"));
    CHECK(refused(swap("\"uri\":\"urn:sonora:test#gain\"", "\"uri\":\"\""), "names no plugin"));
    CHECK(refused(swap("\"name\":\"Gain", "\"name\":\"Gain\",\"name\":\""), "twice"));
    CHECK(refused(good + "x", "after the record"));
    CHECK(refused(good.substr(0, good.size() - 3), ""));
    CHECK(refused("", "ends early"));
    CHECK(refused("[1]", "not a JSON object"));
    // A refusal must say why.
    Record r = sample();
    r.outcome = Outcome::Refused;
    r.reason = "x";
    std::string no_reason = encode(r);
    no_reason.replace(no_reason.find(",\"reason\":\"x\""), 13, "");
    CHECK(refused(no_reason, "no reason"));
    // A usable record gives none.
    CHECK(refused(swap("\"ok\":true,", "\"ok\":true,\"reason\":\"y\","), "gives a reason"));
    // Over the line cap, refused before it is parsed.
    CHECK(refused(std::string(kMaxRecordBytes + 1, ' '), "over the cap"));
    // Too many labels.
    r = sample();
    r.params[1].steps = uint32_t(kMaxRecordLabels + 1);
    r.params[1].labels.assign(kMaxRecordLabels + 1, "l");
    CHECK(refused(encode(r), "more than"));
}

void version_line() {
    uint32_t p = 0;
    CHECK(parse_probe_version("sonora-plugin-host layout 1 clock 1 wire 1 probe 1", p) && p == 1);
    CHECK(parse_probe_version("sonora-plugin-host probe 7 layout 1 clock 1 wire 1\n", p) && p == 7);
    CHECK(!parse_probe_version("sonora-plugin-host layout 1 clock 1 wire 1", p));
    CHECK(!parse_probe_version("sonora-plugin-host layout 1 probe 1 probe 1", p));
    CHECK(!parse_probe_version("sonora-plugin-host layout 1 probe x", p));
}

void fuzz() {
    std::mt19937 rng(7);
    const std::string good = encode(sample());
    Record back;
    std::string e;
    for (int i = 0; i < 20000; ++i) {
        std::string s = good;
        const int edits = 1 + int(rng() % 4);
        for (int k = 0; k < edits; ++k) {
            const std::size_t at = rng() % s.size();
            switch (rng() % 3) {
                case 0: s[at] = char(rng() % 256); break;
                case 1: s.erase(at, 1 + rng() % 8); break;
                default: s.insert(at, 1, "{}[]\",:\\0e"[rng() % 10]); break;
            }
            if (s.empty()) s = "{";
        }
        decode(s, back, e);  // must not crash; the result does not matter
    }
    // Deep nesting is refused, not recursed into without bound.
    CHECK(!decode(std::string(100000, '[') + std::string(100000, ']'), back, e));
}

}  // namespace

int main() {
    golden();
    round_trips();
    refusals();
    version_line();
    fuzz();
    if (failures) {
        std::printf("test_probe_record: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_probe_record: ok\n");
    return 0;
}
