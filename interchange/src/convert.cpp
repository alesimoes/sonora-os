// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>

#include "sonora/interchange/interchange.h"

namespace sonora::interchange {

namespace fs = std::filesystem;

namespace {

const char* kUsage =
    "usage: sonora-convert smf-import <in.mid> <out.sonora>\n"
    "       sonora-convert smf-export <in.sonora> <out.mid>\n"
    "       sonora-convert session    <in.sonora> <out.sonora>\n"
    "\n"
    "smf-import   a Standard MIDI File to a new session of MIDI tracks\n"
    "smf-export   the MIDI and instrument tracks of a session to a format 1 file\n"
    "session      a session rewritten in the current format version\n"
    "\n"
    "Offline: never run against a session an engine has open. What did not\n"
    "survive is listed on standard output. Exit 0 done, 1 failed, 2 usage.\n";

void print_report(const ConversionReport& r, std::string& out) {
    for (const std::string& d : r.dropped) out += "dropped: " + d + "\n";
    for (const std::string& a : r.approximated) out += "approximated: " + a + "\n";
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
    const std::string& command = args[0];
    const std::string& in = args[1];
    const std::string& dest = args[2];
    ConversionReport report;
    std::string error;
    model::Snapshot snapshot;
    bool ok = false;
    if (command == "smf-import") {
        SmfImporter importer;
        ok = importer.can_read(in) ? importer.read(in, snapshot, report, error) &&
                                         write_session(dest, snapshot, report, error)
                                   : (error = in + ": not a Standard MIDI File", false);
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
