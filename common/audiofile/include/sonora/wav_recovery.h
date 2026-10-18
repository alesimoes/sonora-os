// SPDX-License-Identifier: GPL-3.0-or-later
//
// Takes the engine did not live to close (session-format.md, "Crash
// recovery"). A take's header says what its last checkpoint had written,
// or nothing if none ran (WavWriter::checkpoint()); the file may hold more.
// Recovery gives the header the length on disk: the one write to a closed
// audio file the format allows.
//
// Not for the audio thread: it reads, writes and syncs files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonora {

struct TakeRecovery {
    enum class Outcome {
        // Finished: the sizes describe the whole file, or the RIFF size is
        // one only close() or another program writes (a take closed after
        // a failure, a chunk a metadata tool appended). Not touched.
        Intact,
        // Sizes rewritten to the whole frames on disk; a trailing part of
        // a frame dropped, an odd chunk padded. Or, for a file killed
        // between the header's two writes, the RIFF size alone.
        Repaired,
        // The header counts more audio than the file holds. Not touched:
        // what is missing cannot be recovered, and the reader plays what
        // is there.
        Short,
        // Not written by this engine ("Sonora Engine" in bext Originator).
        // Not touched: another program's WAV may carry chunks after its
        // data, which would look like audio the header does not count.
        NotOurs,
        // Not a WAV file the engine could have written, or one whose
        // header was never completed. Not touched.
        Unreadable,
        // Needed repairing, and the rewrite failed; `why` says how.
        Failed,
    };
    std::string path;
    Outcome outcome = Outcome::Unreadable;
    uint64_t header_frames = 0;  // what the data size said, before
    uint64_t frames = 0;         // what it says now
    std::string why;             // Short, Unreadable, Failed
};

TakeRecovery recover_take(const std::string& path);

// recover_take() on every ".wav" in `audio_dir`, in name order. Lists the
// Repaired, Short and Failed ones; the rest need no word to the operator. A folder
// that does not exist yet has nothing to recover.
std::vector<TakeRecovery> recover_takes(const std::string& audio_dir);

}  // namespace sonora
