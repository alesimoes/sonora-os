// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Snapshot to a neutral AafDocument (ADR-0161). Pure: no file, no libaaf.

#include <string>

#include "sonora/interchange/aaf_document.h"
#include "sonora/interchange/interchange.h"

namespace sonora::interchange {

// One composition mob: an audio track is a slot, its active playlist a
// Sequence of SourceClips (a gap is a Filler, a crossfade a Transition between
// its clips), with fades, clip gain and the session's markers (on the first
// slot). Each source a region uses is a source mob; a source with no UMID gets
// a generated one, reported in `approximated`. Every position is in samples
// and the edit rate is the session rate as {rate, 1}; the timecode start is
// converted by integer rationals. What has no AAF place here (automation,
// plugins, sends, the mix, MIDI, other playlists) goes to `report.dropped`.
// False with `error` set only for a session that cannot be exported.
bool map_to_aaf(const model::Snapshot& s, AafDocument& out, ConversionReport& report,
                std::string& error);

}  // namespace sonora::interchange
