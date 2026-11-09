// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built in place of rubberband_stretcher.cpp where pkg-config finds no
// Rubber Band: no stretcher, so audio.elastic stays out of the catalog.
#include "sonora/elastic/library_stretcher.h"

namespace sonora::elastic {

StretcherFactory library_stretcher() { return {}; }

}  // namespace sonora::elastic
