// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol 71 (ADR-0204): the engine settings pair round-trips, a malformed
// one is refused with a reason, and a value out of range is refused, never
// clamped, by validate() and by the fake that tests inject.
#include <cstdio>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/api/engine_settings.h"
#include "sonora/api/fake_engine_settings.h"
#include "sonora/api/wire.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

using namespace sonora::api;

namespace {

bool mentions(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

EngineSettings sample() {
    EngineSettings s;
    s.autosave_interval_s = 300;
    s.backup_count = 7;
    s.automatch_ms = 1200;
    s.device_id = "alsa_card.usb-UMC1820, DEV=0";
    s.buffer_frames = 128;
    return s;
}

int round_trip() {
    CHECK(kProtocolVersion == 71);
    std::string bytes;
    std::string error;

    EngineSettingsSet set;
    set.seq = 0x0102030405060708ull;
    set.settings = sample();
    wire::encode(set, bytes);
    wire::Type type{};
    CHECK(wire::peek_type(bytes, type) && type == wire::Type::EngineSettingsSet);
    EngineSettingsSet set_back;
    CHECK(wire::decode(bytes, set_back, error));
    CHECK(set_back.seq == set.seq && set_back.settings == set.settings);

    // Out of range still decodes: the engine refuses it in words, the
    // connection is not dropped for it.
    set.settings.automatch_ms = 9;
    wire::encode(set, bytes);
    CHECK(wire::decode(bytes, set_back, error));
    CHECK(set_back.settings.automatch_ms == 9);

    EngineSettingsReport report;
    report.seq = 42;
    report.settings = sample();
    report.refusal = SettingsRefusal::BufferFrames;
    report.reason = "buffer of 9 frames is outside";
    report.pending = kSettingsPendingRestart | kSettingsPendingSessionOpen;
    wire::encode(report, bytes);
    EngineSettingsReport report_back;
    CHECK(wire::decode(bytes, report_back, error));
    CHECK(report_back.seq == 42 && report_back.settings == report.settings);
    CHECK(report_back.refusal == SettingsRefusal::BufferFrames);
    CHECK(report_back.reason == report.reason && report_back.pending == report.pending);

    // A client takes the report into its inbox; it refuses the set, which
    // only an engine receives.
    Inbox inbox;
    CHECK(wire::decode_into(bytes, inbox, error));
    CHECK(inbox.settings.size() == 1 && inbox.settings[0].seq == 42);
    inbox.clear();
    CHECK(inbox.settings.empty());
    wire::encode(set, bytes);
    CHECK(!wire::decode_into(bytes, inbox, error));
    CHECK(mentions(error, "not one an engine sends"));
    return 0;
}

int malformed() {
    std::string bytes;
    std::string error;
    EngineSettingsReport report;
    report.settings = sample();
    EngineSettingsReport back;

    wire::encode(report, bytes);
    CHECK(!wire::decode(bytes.substr(0, bytes.size() - 1), back, error));
    CHECK(mentions(error, "cut short"));
    CHECK(!wire::decode(bytes + "x", back, error));
    CHECK(mentions(error, "left over"));
    EngineSettingsSet wrong;
    CHECK(!wire::decode(bytes, wrong, error));
    CHECK(mentions(error, "was expected"));

    report.refusal = SettingsRefusal::DeviceId;   // and no reason
    wire::encode(report, bytes);
    CHECK(!wire::decode(bytes, back, error));
    CHECK(mentions(error, "no reason"));
    report.refusal = SettingsRefusal::None;
    report.reason = "stray words";
    wire::encode(report, bytes);
    CHECK(!wire::decode(bytes, back, error));
    CHECK(mentions(error, "no refusal"));

    report.reason.clear();
    report.refusal = static_cast<SettingsRefusal>(kLastSettingsRefusal + 1);
    report.reason = "a code from a newer engine";
    wire::encode(report, bytes);
    CHECK(!wire::decode(bytes, back, error));
    CHECK(mentions(error, "unknown here"));

    report.refusal = SettingsRefusal::None;
    report.reason.clear();
    report.pending = 1u << 2;
    wire::encode(report, bytes);
    CHECK(!wire::decode(bytes, back, error));
    CHECK(mentions(error, "pending flags"));

    EngineSettingsSet set;
    set.settings.device_id.assign(kMaxDeviceIdBytes + 1, 'a');
    wire::encode(set, bytes);
    EngineSettingsSet set_back;
    CHECK(!wire::decode(bytes, set_back, error));
    CHECK(mentions(error, "over the cap"));
    return 0;
}

int validation() {
    std::string why;
    CHECK(validate(EngineSettings{}, why) == SettingsRefusal::None && why.empty());
    CHECK(validate(sample(), why) == SettingsRefusal::None);

    EngineSettings edges;
    edges.autosave_interval_s = 0;
    edges.backup_count = 0;
    edges.automatch_ms = kMinAutomatchMs;
    edges.buffer_frames = 0;
    CHECK(validate(edges, why) == SettingsRefusal::None);
    edges.autosave_interval_s = kMaxAutosaveIntervalS;
    edges.backup_count = kMaxBackupCount;
    edges.automatch_ms = kMaxAutomatchMs;
    edges.buffer_frames = kMaxBufferFrames;
    edges.device_id.assign(kMaxDeviceIdBytes, 'd');
    CHECK(validate(edges, why) == SettingsRefusal::None);

    EngineSettings s = sample();
    s.autosave_interval_s = kMaxAutosaveIntervalS + 1;
    CHECK(validate(s, why) == SettingsRefusal::AutosaveInterval);
    CHECK(mentions(why, "autosave interval 86401 s"));
    s = sample();
    s.backup_count = kMaxBackupCount + 1;
    CHECK(validate(s, why) == SettingsRefusal::BackupCount && mentions(why, "1001"));
    s = sample();
    s.automatch_ms = kMinAutomatchMs - 1;
    CHECK(validate(s, why) == SettingsRefusal::AutomatchTime && mentions(why, "9 ms"));
    s.automatch_ms = kMaxAutomatchMs + 1;
    CHECK(validate(s, why) == SettingsRefusal::AutomatchTime);
    s = sample();
    s.device_id = "hw:0\n";
    CHECK(validate(s, why) == SettingsRefusal::DeviceId && mentions(why, "control byte 10"));
    s.device_id.assign(kMaxDeviceIdBytes + 1, 'd');
    CHECK(validate(s, why) == SettingsRefusal::DeviceId);
    s = sample();
    s.buffer_frames = kMinBufferFrames - 1;
    CHECK(validate(s, why) == SettingsRefusal::BufferFrames && mentions(why, "15 frames"));
    s.buffer_frames = kMaxBufferFrames + 1;
    CHECK(validate(s, why) == SettingsRefusal::BufferFrames);
    return 0;
}

int fake_store() {
    FakeEngineSettings fake;
    const EngineSettings defaults = fake.held();
    CHECK(fake.report().seq == 0 && fake.report().pending == 0);

    EngineSettingsSet bad;
    bad.seq = 5;
    bad.settings = sample();
    bad.settings.automatch_ms = kMaxAutomatchMs + 1;
    EngineSettingsReport r = fake.set(bad);
    // Refused whole, with the reason, and not clamped to 5000: nothing held
    // moved, not even the values that were in range.
    CHECK(r.seq == 5 && r.refusal == SettingsRefusal::AutomatchTime);
    CHECK(!r.reason.empty() && mentions(r.reason, "AutoMatch time 5001 ms"));
    CHECK(r.settings == defaults && fake.held() == defaults);
    CHECK(fake.refusals() == 1 && fake.last_reason() == r.reason);

    // The refusal crosses the wire in words.
    std::string bytes;
    std::string error;
    wire::encode(r, bytes);
    EngineSettingsReport back;
    CHECK(wire::decode(bytes, back, error) && back.reason == r.reason);

    EngineSettingsSet good;
    good.seq = 6;
    good.settings = sample();
    r = fake.set(good);
    CHECK(r.seq == 6 && r.refusal == SettingsRefusal::None && r.reason.empty());
    CHECK(r.settings == good.settings && fake.held() == good.settings);
    // AutoMatch, device and buffer all differ from what is running.
    CHECK(r.pending == (kSettingsPendingSessionOpen | kSettingsPendingRestart));
    fake.open_session();
    CHECK(fake.report().pending == kSettingsPendingRestart);
    fake.restart();
    CHECK(fake.report().pending == 0);

    fake.fail_store(true);
    good.settings.backup_count = 3;
    r = fake.set(good);
    CHECK(r.refusal == SettingsRefusal::NotStored && !r.reason.empty());
    CHECK(fake.held().backup_count == 7 && fake.refusals() == 2);
    return 0;
}

// A control with no settings path answers no seq, so an interface holds a
// notice rather than waiting for a report that will not come.
class NoSettingsControl final : public EngineControl {
public:
    bool connect(std::string&) override { return true; }
    const Hello& hello() const override { return hello_; }
    uint64_t send(const CommandRequest&) override { return 1; }
    uint64_t send(const ParameterChange&) override { return 1; }
    uint64_t send(const PluginParameterChange&) override { return 1; }
    uint64_t submit(const ModelRequest&) override { return 1; }
    void poll(Inbox&) override {}
    StateView& state() override { return *view_; }   // never called here

private:
    Hello hello_;
    StateView* view_ = nullptr;
};

int default_control() {
    NoSettingsControl control;
    EngineControl& e = control;
    CHECK(e.send_settings(EngineSettingsSet{}) == 0);
    return 0;
}

}  // namespace

int main() {
    if (round_trip() != 0) return 1;
    if (malformed() != 0) return 1;
    if (validation() != 0) return 1;
    if (fake_store() != 0) return 1;
    if (default_control() != 0) return 1;
    std::puts("engine settings wire: ok");
    return 0;
}
