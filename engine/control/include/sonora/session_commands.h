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
struct PunchRange;

// Asked before every take; false refuses it, with `why` shown to the
// operator. The session knows nothing of what feeds its inputs, so the
// caller says whether recording now would capture anything.
using MayRecord = std::function<bool(std::string& why)>;

// Asked as each take starts, after MayRecord: true, with `range` set, makes
// it a punch (Session::start_punch()) instead of a whole take. The session
// does not hold the document the punch range is saved in.
using PunchFor = std::function<bool(PunchRange& range)>;

// `take` is the number the next take will be written under; `quit` is set by
// app.quit. Both are owned by the caller and must outlive `reg`, as must
// `may_record`, `punch_for` and what they refer to. Without `may_record`
// every take may start; without `punch_for` every take is whole.
// With `playback` (which must outlive `reg` too), transport.play,
// transport.toggle_play and transport.return_to_zero are installed as well,
// gated on the plan's residency through it, and a take started from a key
// waits behind the same gate.
void install_session_commands(CommandRegistry& reg, Session& session, int& take,
                              bool& quit, MayRecord may_record = nullptr,
                              PlaybackController* playback = nullptr,
                              PunchFor punch_for = nullptr);

}  // namespace sonora
