// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the engine's entry point asks of the OS before it opens a session:
// pin its memory, hear the signals that ask it to stop, and find where
// sessions live (ADR-0008 §3). Main thread, before the audio thread starts;
// nothing here is for the callback, except quit_requested(), which is one
// atomic load.
#pragma once

#include <string>

namespace sonora::platform {

// Locks every page the process has, and every page it maps from now on, into
// RAM (mlockall MCL_CURRENT | MCL_FUTURE on Linux), so the audio thread never
// faults on the session's buffers. Call before the session is constructed.
//
// Needs RLIMIT_MEMLOCK unlimited, and raises the soft limit to the hard one
// to get it. With a finite limit MCL_FUTURE would turn the first mapping past
// it -- a thread stack, a capture ring -- into a failed allocation later, so
// that is refused here instead, with `why` naming the limit. A platform with
// no implementation refuses too, and says so in `why`.
bool lock_memory(std::string& why);

// Bytes the process holds locked, in kB; -1 if it cannot be read or the
// platform cannot tell.
long locked_kb();

// The signals that ask the engine to stop, turned into a flag the interface
// loop polls. The loop then calls session.stop(), which is what finalises the
// WAV headers; a signal left at its default action kills the process first.
// Linux: SIGINT, SIGTERM and SIGHUP. Elsewhere: whatever standard C offers
// (SIGINT, SIGTERM). The handler only sets an async-signal-safe flag.
void install_quit_handlers();

bool quit_requested();

// For tests, which raise each signal in turn.
void clear_quit_request();

// Where a session folder is made when the operator names none: the home
// directory's "Sessions". `home` is the value of the environment's home
// setting, or null when it is unset; the working directory stands in then.
// A parameter rather than a getenv here so a test can pass its own.
std::string default_session_root(const char* home);

}  // namespace sonora::platform
