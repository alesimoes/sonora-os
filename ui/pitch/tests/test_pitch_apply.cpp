// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apply against the fake engine: the capability read from its Hello, the
// sample.edit text the edits come to, the engine's refusal held as a notice,
// an accepted render making the notes stale, and every refusal before
// anything is sent.
#include <memory>

#include "sonora/api/fake_engine.h"
#include "sonora/ui_pitch/pitch_engine.h"

#include "pitch_test_support.h"

using namespace sonora;
using namespace sonora::ui_pitch;
using namespace pitchtest;

namespace {

// Sends through the fake engine as the interface's link does.
class FakeEngineSink : public PitchCommandSink {
public:
    explicit FakeEngineSink(api::testing::FakeEngine& engine) : engine_(engine) {}
    uint64_t send(const std::string& command, const std::string& argument,
                  std::string&) override {
        ++sent;
        api::CommandRequest r;
        r.command = command;
        r.argument = argument;
        last_seq = engine_.send(r);
        return last_seq;
    }
    int sent = 0;
    uint64_t last_seq = 0;

private:
    api::testing::FakeEngine& engine_;
};

// The replies the engine has for us, handed to the model.
int pump(api::testing::FakeEngine& engine, PitchEditorModel& model) {
    api::Inbox in;
    engine.poll(in);
    for (const api::CommandReply& r : in.replies) {
        model.on_reply(r.seq, r.result == api::DispatchResult::Ok, r.message);
    }
    return int(in.replies.size());
}

std::unique_ptr<api::testing::FakeEngine> engine_with_pitch(bool pitch) {
    api::testing::FakeEngine::Config config;
    if (pitch) config.capabilities.push_back(api::kCapabilityPitch);
    auto engine = std::make_unique<api::testing::FakeEngine>(config);
    std::string error;
    if (!engine->connect(error)) std::fprintf(stderr, "connect: %s\n", error.c_str());
    return engine;
}

int test_capability_absent() {
    auto engine = engine_with_pitch(false);
    const PitchCapability c =
        pitch_capability(engine->hello().implemented_commands, engine->hello().capabilities);
    CHECK(!c.available);
    CHECK(c.reason.find("without a pitch shifter") != std::string::npos);
    // sample.edit missing altogether says so instead.
    const PitchCapability none = pitch_capability({}, {api::kCapabilityPitch});
    CHECK(!none.available && none.reason.find("sample.edit") != std::string::npos);

    FakeEngineSink sink(*engine);
    PitchEditorModel m(&sink);
    m.set_capability(c);
    std::string why;
    CHECK(m.open(region(), three_notes(), why));
    CHECK(m.begin_drag(1, why) && m.drag_to(100.0, why));
    CHECK(!m.apply(why));
    CHECK(why == c.reason && m.notice() == c.reason);
    CHECK(sink.sent == 0 && !m.pending());
    return 0;
}

int test_apply_text() {
    auto engine = engine_with_pitch(true);
    const PitchCapability c =
        pitch_capability(engine->hello().implemented_commands, engine->hello().capabilities);
    CHECK(c.available && c.reason.empty());
    FakeEngineSink sink(*engine);
    PitchEditorModel m(&sink);
    m.set_capability(c);
    std::string why;
    CHECK(m.open(region(), three_notes(), why));
    CHECK(m.begin_drag(1, why) && m.drag_to(100.0, why));
    m.end_drag();
    CHECK(m.select(2) && m.set_gain(-3.5, why));
    CHECK(m.apply(why));
    CHECK(sink.sent == 1 && m.pending());
    // Untouched A3 is left out; the frames are the region's, the range the
    // timeline's.
    CHECK(m.last_argument() == "pitch 7 48000 192000 all n=48000:72000:100;n=96000:120000:0:0:-3.5");
    api::PitchEditSpec spec;
    const std::string param = m.last_argument().substr(m.last_argument().rfind(' ') + 1);
    CHECK(api::parse_pitch_param(param, spec, why) && spec.notes.size() == 2);
    // A second Apply before the reply is refused, and nothing more is sent.
    CHECK(!m.apply(why));
    CHECK(why.find("still being rendered") != std::string::npos);
    CHECK(sink.sent == 1);
    return 0;
}

int test_engine_refusal_is_held() {
    auto engine = engine_with_pitch(true);
    FakeEngineSink sink(*engine);
    PitchEditorModel m(&sink);
    m.set_capability(pitch_capability(engine->hello().implemented_commands,
                                      engine->hello().capabilities));
    std::string why;
    CHECK(m.open(region(), three_notes(), why));
    CHECK(m.begin_drag(0, why) && m.drag_to(-200.0, why));
    m.end_drag();
    CHECK(m.apply(why));
    CHECK(pump(*engine, m) == 1);
    // The fake has no audio files and says so; the edit is kept for another try.
    CHECK(!m.pending() && !m.stale());
    CHECK(m.notice().find("pitch: not applied: ") == 0);
    CHECK(m.notice().find("no audio files") != std::string::npos);
    CHECK(m.edit(0).shift_cents == -200.0);
    CHECK(m.begin_drag(0, why));
    return 0;
}

int test_accepted_render_makes_notes_stale() {
    auto engine = engine_with_pitch(true);
    FakeEngineSink sink(*engine);
    PitchEditorModel m(&sink);
    m.set_capability({true, ""});
    std::string why;
    CHECK(m.open(region(), three_notes(), why));
    CHECK(m.begin_drag(2, why) && m.drag_to(100.0, why));
    CHECK(m.apply(why));
    const uint64_t seq = sink.last_seq;
    m.on_reply(seq + 5, true, "");  // someone else's reply changes nothing
    CHECK(m.pending());
    m.on_reply(seq, true, "");
    CHECK(!m.pending() && m.stale());
    CHECK(m.status().find("one undo restores the take") != std::string::npos);
    CHECK(!m.begin_drag(0, why));
    CHECK(why.find("analyse it again") != std::string::npos);
    CHECK(!m.apply(why) && !m.reset_edits(why));
    CHECK(sink.sent == 1);
    // Opening the region again starts over.
    CHECK(m.open(region(), three_notes(), why) && !m.stale() && m.begin_drag(0, why));
    return 0;
}

int test_refused_before_sending() {
    auto engine = engine_with_pitch(true);
    FakeEngineSink sink(*engine);
    std::string why;
    // Nothing edited.
    PitchEditorModel m(&sink);
    m.set_capability({true, ""});
    CHECK(m.open(region(), three_notes(), why));
    CHECK(!m.apply(why) && why == "pitch: nothing to change");
    // No engine link.
    PitchEditorModel unlinked;
    unlinked.set_capability({true, ""});
    CHECK(unlinked.open(region(), three_notes(), why));
    CHECK(unlinked.begin_drag(0, why) && unlinked.drag_to(100.0, why));
    CHECK(!unlinked.apply(why) && why.find("no engine") != std::string::npos);
    // Nothing open.
    PitchEditorModel closed(&sink);
    closed.set_capability({true, ""});
    CHECK(!closed.apply(why));
    CHECK(sink.sent == 0);
    return 0;
}

int test_session_reset_drops_pending_reply() {
    auto engine = engine_with_pitch(true);
    FakeEngineSink sink(*engine);
    PitchEditorModel m(&sink);
    m.set_capability({true, ""});
    std::string why;
    CHECK(m.open(region(), three_notes(), why));
    CHECK(m.begin_drag(0, why) && m.drag_to(100.0, why));
    CHECK(m.apply(why));
    m.reset_session();
    CHECK(pump(*engine, m) == 1);
    // The last session's refusal does not land on the new one.
    CHECK(m.notice().empty() && !m.pending());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_capability_absent();
    failed += test_apply_text();
    failed += test_engine_refusal_is_held();
    failed += test_accepted_render_makes_notes_stale();
    failed += test_refused_before_sending();
    failed += test_session_reset_drops_pending_reply();
    if (failed == 0) std::printf("ui_pitch_apply: ok\n");
    return failed == 0 ? 0 : 1;
}
