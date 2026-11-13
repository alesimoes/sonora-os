// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>

#include "sonora/audio_import.h"
#include "sonora/interchange/aaf_import.h"
#include "sonora/interchange/interchange.h"

namespace sonora::interchange {

namespace fs = std::filesystem;

namespace {

const char* kUsage =
    "usage: sonora-convert smf-import <in.mid> <out.sonora>\n"
    "       sonora-convert aaf-import <in.aaf> <out.sonora>\n"
    "       sonora-convert import     <in.mid|in.aaf> <out.sonora>\n"
    "       sonora-convert smf-export <in.sonora> <out.mid>\n"
    "       sonora-convert session    <in.sonora> <out.sonora>\n"
    "\n"
    "smf-import   a Standard MIDI File to a new session of MIDI tracks\n"
    "aaf-import   an AAF composition; its essence is copied into Audio Files/\n"
    "import       smf-import or aaf-import, chosen by the file's extension\n"
    "smf-export   the MIDI and instrument tracks of a session to a format 1 file\n"
    "session      a session rewritten in the current format version\n"
    "\n"
    "Offline: never run against a session an engine has open. What did not\n"
    "survive is listed on standard output. Exit 0 done, 1 failed, 2 usage.\n";

void print_report(const ConversionReport& r, std::string& out) {
    for (const std::string& d : r.dropped) out += "dropped: " + d + "\n";
    for (const std::string& a : r.approximated) out += "approximated: " + a + "\n";
}

bool has_ext(const std::string& p, const char* ext) {
    const std::string e = ext;
    return p.size() > e.size() &&
           std::equal(e.rbegin(), e.rend(), p.rbegin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

// The mapper names a source "Audio Files/<name>"; the file itself sits beside
// the .aaf, in its own Audio Files/ or Media/, or at the path the AAF gave.
bool find_essence(const fs::path& aaf_dir, const std::string& rel, fs::path& found) {
    const std::string base = fs::path(rel).filename().string();
    for (const fs::path& c : {aaf_dir / rel, aaf_dir / base, aaf_dir / "Audio Files" / base,
                              aaf_dir / "Media" / base}) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) {
            found = c;
            return true;
        }
    }
    return false;
}

// A missing file is an error naming it, never a silent gap.
bool copy_essence(const std::string& aaf, const std::string& dest, const model::Snapshot& snap,
                  std::string& error) {
    if (!snap.sources) return true;
    const fs::path aaf_dir = fs::absolute(fs::path(aaf)).parent_path();
    const fs::path folder = fs::absolute(fs::path(dest)).parent_path();
    for (const model::Source& s : snap.sources->sources) {
        if (s.kind != model::SourceKind::Imported) continue;
        fs::path from;
        if (!find_essence(aaf_dir, s.path, from)) {
            error = "AAF essence file not found: " + s.path;
            return false;
        }
        const fs::path to = folder / fs::path(s.path);
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        ImportResult r;
        std::string why;
        if (!import_audio(from.string(), to.string(), snap.settings.sample_rate, nullptr, nullptr,
                          r, why)) {
            error = from.string() + ": " + why;
            return false;
        }
    }
    return true;
}

bool aaf_import(const std::string& in, const std::string& dest, model::Snapshot& snap,
                ConversionReport& report, std::string& error) {
    std::unique_ptr<AafReader> reader(make_libaaf_reader());
    if (!reader) {
        error = "built without AAF support (CMake option SONORA_WITH_LIBAAF is off)";
        return false;
    }
    AafImporter importer(reader.get());
    if (!importer.can_read(in)) {
        error = in + ": not an AAF file";
        return false;
    }
    return importer.read(in, snap, report, error) && copy_essence(in, dest, snap, error) &&
           write_session(dest, snap, report, error);
}

}  // namespace

int run_convert(const std::vector<std::string>& args, std::string& out, std::string& err) {
    if (args.size() == 1 && (args[0] == "-h" || args[0] == "--help")) {
        out += kUsage;
        return 0;
    }
    if (args.size() != 3) {
        err += kUsage;
        return 2;
    }
    std::string command = args[0];
    const std::string& in = args[1];
    const std::string& dest = args[2];
    if (command == "import") command = has_ext(in, ".aaf") ? "aaf-import" : "smf-import";
    ConversionReport report;
    std::string error;
    model::Snapshot snapshot;
    bool ok = false;
    if (command == "smf-import") {
        SmfImporter importer;
        ok = importer.can_read(in) ? importer.read(in, snapshot, report, error) &&
                                         write_session(dest, snapshot, report, error)
                                   : (error = in + ": not a Standard MIDI File", false);
    } else if (command == "aaf-import") {
        ok = aaf_import(in, dest, snapshot, report, error);
    } else if (command == "smf-export") {
        SmfExporter exporter;
        ok = read_session(in, snapshot, report, error) &&
             exporter.write(snapshot, dest, report, error);
    } else if (command == "session") {
        ok = read_session(in, snapshot, report, error) &&
             write_session(dest, snapshot, report, error,
                           fs::absolute(fs::path(in)).parent_path().string());
    } else {
        err += "sonora-convert: unknown command \"" + command + "\"\n" + kUsage;
        return 2;
    }
    if (!ok) {
        err += "sonora-convert: " + error + "\n";
        return 1;
    }
    print_report(report, out);
    out += "wrote " + dest + "\n";
    return 0;
}

}  // namespace sonora::interchange
