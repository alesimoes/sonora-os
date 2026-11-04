// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Interchange (ADR-0007 section 4.5, M10): other formats to and from our
// native session, offline. Nothing here is reachable from the audio thread,
// and nothing touches a running engine; the only caller is sonora-convert,
// a short-lived process, so a hostile file can crash a converter and not a
// session. It allocates and uses std::string freely.

#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::interchange {

// Honest about what did not survive.
struct ConversionReport {
    std::vector<std::string> dropped, approximated;
};

// The ADR's `model::Session` is a model::Snapshot.
class Importer {
public:
    virtual ~Importer() = default;
    virtual bool can_read(const std::string& path) const = 0;
    virtual bool read(const std::string& path, model::Snapshot& out, ConversionReport&,
                      std::string& error) = 0;
};

class Exporter {
public:
    virtual ~Exporter() = default;
    virtual bool write(const model::Snapshot&, const std::string& path, ConversionReport&,
                       std::string& error) = 0;
};

// Standard MIDI File (formats 0 and 1) to a new session of MIDI tracks, one
// per file track that holds notes or events, each one region at tick 0; the
// file's tempo and time-signature events become the tempo map at 48 kHz.
class SmfImporter : public Importer {
public:
    bool can_read(const std::string& path) const override;
    bool read(const std::string& path, model::Snapshot& out, ConversionReport&,
              std::string& error) override;
    // The same from bytes in memory, for tests.
    bool read_bytes(const std::string& bytes, const std::string& name, model::Snapshot& out,
                    ConversionReport&, std::string& error);
};

// A session's MIDI and instrument tracks to a format 1 file at 960 ppq: a
// conductor track with the tempo map, then one track per MIDI track. Audio,
// markers and the mix are dropped, and the report says so.
class SmfExporter : public Exporter {
public:
    bool write(const model::Snapshot&, const std::string& path, ConversionReport&,
               std::string& error) override;
    bool encode(const model::Snapshot&, std::string& bytes, ConversionReport&,
                std::string& error);
};

// Native session files. `path` is the "<name>.sonora" file; its MIDI
// sequences are read from "MIDI Sequences/" beside it. An older format
// version is migrated on read, so a write is always the current version.
bool read_session(const std::string& path, model::Snapshot& out, ConversionReport&,
                  std::string& error);
// Written as the engine's saver writes it (session_files::SessionFolder), so
// the result opens in the engine. The folder is created if it is not there.
// The plugin state a session names is copied from `state_source` (the input
// session's folder for a rewrite), as the saver does; without it a session
// that names state is refused.
bool write_session(const std::string& path, const model::Snapshot&, ConversionReport&,
                   std::string& error, const std::string& state_source = std::string());

// sonora-convert's command line, apart from main() so a test can run it.
// Returns the exit code: 0 done, 1 the conversion failed, 2 usage.
int run_convert(const std::vector<std::string>& args, std::string& out, std::string& err);

}  // namespace sonora::interchange
