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

class Session;

// Asked before every take; false refuses it, with `why` shown to the
// operator. The session knows nothing of what feeds its inputs, so the
// caller says whether recording now would capture anything.
using MayRecord = std::function<bool(std::string& why)>;

// `take` is the number the next take will be written under; `quit` is set by
// app.quit. Both are owned by the caller and must outlive `reg`, as must
// `may_record` and what it refers to. Without it every take may start.
void install_session_commands(CommandRegistry& reg, Session& session, int& take,
                              bool& quit, MayRecord may_record = nullptr);

}  // namespace sonora
