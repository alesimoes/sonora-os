// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signals that ask the engine to stop, turned into a flag the interface
// loop polls. The loop then calls session.stop(), which is what finalises the
// WAV headers; a signal left at its default action kills the process first.
#pragma once

namespace sonora {

// SIGINT, SIGTERM and SIGHUP. The handler only sets an async-signal-safe flag.
void install_quit_signals();

bool quit_requested();

// For tests, which raise each signal in turn.
void clear_quit_request();

}  // namespace sonora
