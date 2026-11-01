// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands this build wires to a Session.
//
// One definition shared by sonora-engine and test_keymap. The test asks
// whether a profile can arm, record and quit with the handlers that really
// exist; a hand-kept copy of that list in the test is a second source of
// truth, and the day it drifts the test passes about a build nobody ships.
//
// Nothing here is reachable from Session::process(). The handlers run on the
// terminal loop's thread and reach the audio thread only through the atomics
// on Track and Transport.
#pragma once

#include <functional>
#include <string>

#include "sonora/command.h"

namespace sonora {

class PlaybackController;
class Session;
struct LoopRange;
struct PunchRange;

// Asked before every take; false refuses it, with `why` shown to the
// operator. The session knows nothing of what feeds its inputs, so the
// caller says whether recording now would capture anything.
using MayRecord = std::function<bool(std::string& why)>;

// Asked as each take starts, after MayRecord: true, with `range` set, makes
// it a punch (Session::start_punch()) instead of a whole take. The session
// does not hold the document the punch range is saved in.
using PunchFor = std::function<bool(PunchRange& range)>;

// Asked as each take starts, before PunchFor: true, with `loop` set, makes
// it a loop take (Session::start_loop()); false with `refusal` set refuses
// the take, with nothing opened (ADR-0016 C3: loop and punch together);
// false with `refusal` empty lets PunchFor decide.
using LoopFor = std::function<bool(LoopRange& loop, std::string& refusal)>;

// Asked before an engine track is armed: false keeps it disarmed, with
// `why` for the operator. The session does not know which document track
// sits on an engine track, nor its kind; a MIDI or instrument track records
// no audio (ADR-0014 M-1).
using MayArm = std::function<bool(std::size_t engine_track, std::string& why)>;

// `take` is the number the next take will be written under; `quit` is set by
// app.quit. Both are owned by the caller and must outlive `reg`, as must
// `may_record`, `punch_for` and what they refer to. Without `may_record`
// every take may start; without `punch_for` every take is whole; without
// `may_arm` every engine track may be armed.
// With `playback` (which must outlive `reg` too), transport.play,
// transport.toggle_play, transport.return_to_zero and transport.locate are
// installed as well,
// gated on the plan's residency through it, and a take started from a key
// waits behind the same gate.
// Without `loop_for` no take is a loop take; with it, one is located to its
// pre-roll first, and stops on transport.punch as on transport.stop. A
// punch started with the transport stopped is located to its range's
// `start` first the same way, and its `roll_end` stops the transport after
// it. transport.play and transport.toggle_play take an optional range,
// "A-B", played with the pre-roll and post-roll of the snapshot `playback`
// last showed (PlaybackController::play_range()).
// With `playback`, transport.record_ready too: Pro Tools' Record, which sets
// `record_ready` and starts nothing. Play then starts the take as
// transport.record would, and stop, a locate or a take started another way
// clears it (ADR-0009 update of 2026-11-01, record-ready). The caller owns
// `record_ready`, publishes it, and clears it when a session opens; without
// one, the commands keep their own.
void install_session_commands(CommandRegistry& reg, Session& session, int& take,
                              bool& quit, MayRecord may_record = nullptr,
                              PlaybackController* playback = nullptr,
                              PunchFor punch_for = nullptr, MayArm may_arm = nullptr,
                              LoopFor loop_for = nullptr, bool* record_ready = nullptr);

}  // namespace sonora
