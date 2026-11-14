#pragma once

// transport.varispeed on the control thread (ADR-0190). Kept out of
// session_commands.cpp, which is large.

#include <string>

namespace sonora {

class Session;

// Parses `argument`, then Session::set_varispeed (a relaxed atomic; the
// audio thread is untouched). On refusal nothing changes and `why` says so.
bool apply_varispeed(Session& session, const std::string& argument, std::string& why);

}  // namespace sonora
