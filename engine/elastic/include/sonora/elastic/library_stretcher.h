// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stretcher this build has (ADR-0136 §3): Rubber Band (GPL-2.0-or-later)
// when pkg-config found it, else none, and then audio.elastic is not
// installed and its capability not announced, rather than a poor fallback.
#pragma once

#include "sonora/elastic/elastic_job.h"

namespace sonora::elastic {

// Empty when the engine was built without Rubber Band.
StretcherFactory library_stretcher();

}  // namespace sonora::elastic
