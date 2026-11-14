// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp suite's load-time checks (suite_load.cpp), run by suite.cpp.
#pragma once

#include "sonora/measure/measure.h"

namespace sonora::native::suites::amp_load {

// Over the class-E budget: refused with a held notice, never loaded.
void refusal(measure::Suite& s);
// The "model" slot: a .nam by path, a bad path, a refused one, cleared.
void file_slot(measure::Suite& s);
// A capture that needs another latency waits for the next prepare().
void deferred(measure::Suite& s);
// Loads from a control thread while the audio thread renders.
void concurrent_loads(measure::Suite& s);

}  // namespace sonora::native::suites::amp_load
