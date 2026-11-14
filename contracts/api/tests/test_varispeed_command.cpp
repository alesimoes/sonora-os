// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0190: transport.varispeed is a catalog command whose argument is the
// ratio as text. The parser both sides share, then the fake engine over the
// control API: it holds the ratio, refuses what is not one with the engine's
// words and keeps the old ratio, and opens every session at 1.
#include <cstdio>
#include <string>

#include "sonora/api/fake_engine.h"
#include "sonora/api/varispeed.h"
#include "sonora/api/wire.h"
#include "sonora/command.h"
#include "sonora/model/model.h"
#include "sonora/model/session_file.h"

using namespace sonora::api;
using sonora::DispatchResult;
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

void in_the_catalog() {
    const sonora::CommandSpec* spec = sonora::find_command(kVarispeedCommand);
    CHECK(spec != nullptr);
    if (spec == nullptr) return;
    CHECK(spec->takes_argument);
    CHECK(spec->target == sonora::CommandTarget::None);
    CHECK(sonora::is_engine_command(kVarispeedCommand));
}

bool accepts(const char* text, double want) {
    double r = -1.0;
    std::string why;
    return parse_varispeed(text, r, why) && r == want && why.empty();
}

bool refuses(const char* text, const std::string& reason) {
    double r = 1.25;
    std::string why;
    return !parse_varispeed(text, r, why) && r == 1.25 && why == reason;
}

void parser() {
    CHECK(accepts("1", 1.0));
    CHECK(accepts("1.5", 1.5));
    CHECK(accepts("0.5", 0.5));
    CHECK(accepts("2", 2.0));
    CHECK(accepts("2.000000", 2.0));
    CHECK(accepts("0.75", 0.75));
    CHECK(accepts("01.25", 1.25));

    CHECK(refuses("0.49", kVarispeedOutOfRange));
    CHECK(refuses("0.499999", kVarispeedOutOfRange));
    CHECK(refuses("2.000001", kVarispeedOutOfRange));
    CHECK(refuses("-1", kVarispeedOutOfRange));
    CHECK(refuses("0", kVarispeedOutOfRange));
    CHECK(refuses("99999999999999999999999", kVarispeedOutOfRange));

    const std::string not_one = "is not a varispeed ratio such as 1.25";
    for (const char* bad : {"nan", "NaN", "inf", "1e0", "1,5", " 1", "1 ", ".5", "1.", "-",
                            "1.2.3", "0x1", "+1", "1.0000001", "150%"}) {
        double r = 1.25;
        std::string why;
        const bool ok = parse_varispeed(bad, r, why);
        if (ok || r != 1.25 || why != "\"" + std::string(bad) + "\" " + not_one) {
            std::fprintf(stderr, "FAIL \"%s\" read as a ratio, or refused as \"%s\"\n", bad,
                         why.c_str());
            ++failures;
        }
    }
    CHECK(refuses("", "no varispeed ratio given (0.5 to 2.0, 1 resets)"));

    CHECK(format_varispeed(1.0) == "1");
    CHECK(format_varispeed(1.5) == "1.5");
    CHECK(format_varispeed(0.749) == "0.749");
    CHECK(format_varispeed(2.0) == "2");
    for (const char* text : {"0.5", "0.666667", "1.059463", "2"}) {
        double r = 0.0;
        std::string why;
        CHECK(parse_varispeed(text, r, why) && format_varispeed(r) == text);
    }
}

struct Rig {
    FakeEngine engine;
    Inbox in;

    explicit Rig(FakeEngine::Config cfg) : engine(std::move(cfg)) {
        std::string error;
        CHECK(engine.connect(error));
    }

    CommandReply reply_to(uint64_t seq) {
        engine.poll(in);
        for (const CommandReply& c : in.replies) {
            if (c.seq == seq) return c;
        }
        return CommandReply{seq, DispatchResult::UnknownCommand, "no reply"};
    }

    CommandReply varispeed(const std::string& argument) {
        CommandRequest r;
        r.command = kVarispeedCommand;
        r.argument = argument;
        return reply_to(engine.send(r));
    }
};

void fake_engine() {
    FakeEngine::Config cfg;
    cfg.tracks = 2;
    Rig rig(cfg);
    FakeEngine& e = rig.engine;
    CHECK(e.varispeed() == 1.0);

    CHECK(rig.varispeed("1.5").result == DispatchResult::Ok);
    CHECK(e.varispeed() == 1.5);

    // Refused, in the engine's words, and the ratio that was kept.
    CommandReply r = rig.varispeed("0.49");
    CHECK(r.result == DispatchResult::Failed && r.message == kVarispeedOutOfRange);
    r = rig.varispeed("NaN");
    CHECK(r.result == DispatchResult::Failed &&
          r.message == "\"NaN\" is not a varispeed ratio such as 1.25");
    r = rig.varispeed("");
    CHECK(r.result == DispatchResult::Failed && !r.message.empty());
    CHECK(e.varispeed() == 1.5);

    // Allowed while rolling, as Session::set_varispeed is.
    CommandRequest play;
    play.command = "transport.play";
    CHECK(rig.reply_to(e.send(play)).result == DispatchResult::Ok);
    CHECK(rig.varispeed("0.5").result == DispatchResult::Ok);
    CHECK(e.varispeed() == 0.5);

    CHECK(rig.varispeed("1").result == DispatchResult::Ok);
    CHECK(e.varispeed() == 1.0);
    CHECK(rig.varispeed("2").result == DispatchResult::Ok);

    // Not saved: the next session opens at normal speed.
    CommandRequest stop;
    stop.command = "transport.stop";
    CHECK(rig.reply_to(e.send(stop)).result == DispatchResult::Ok);
    sonora::model::SessionSettings settings;
    settings.name = "Next";
    settings.session_id = sonora::model::new_session_id();
    settings.sample_rate = cfg.sample_rate;
    std::string bytes;
    std::string error;
    CHECK(sonora::model::serialize(sonora::model::new_session(settings),
                                   sonora::model::WriteInfo{"test", 1}, bytes, error));
    ModelRequest load;
    load.action = ModelAction::Load;
    load.op = wire::encode_load("", bytes);
    CHECK(rig.reply_to(e.submit(load)).result == DispatchResult::Ok);
    CHECK(e.varispeed() == 1.0);
}

}  // namespace

int main() {
    in_the_catalog();
    parser();
    fake_engine();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("test_varispeed_command: ok");
    return 0;
}
