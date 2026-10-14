// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the SPICE reference harnesses share (spice.cpp, the triode stage;
// spice_opto.cpp, the optical cell): number formatting, files, and running
// ngspice in batch mode. Offline only; never on an audio thread.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "sonora/json/json.h"

namespace sonora::measure::spice_util {

inline std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}

// Numbers in a netlist: %.10g is exact for every value we use and reads
// the same to a person.
inline std::string sp(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

inline json::Value jnum(double v) { return json::Value::number_lexeme(num(v)); }

inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

inline bool write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

// Runs one case in `work`; returns false with a message on failure.
inline bool run_ngspice(const std::string& ngspice, const std::string& work, const std::string& netlist,
                        std::string& error) {
    const std::string cir = work + "/case.cir";
    if (!write_file(cir, netlist)) {
        error = "cannot write " + cir;
        return false;
    }
    std::remove((work + "/out.txt").c_str());
    std::remove((work + "/op.txt").c_str());
    const std::string cmd = "cd '" + work + "' && '" + ngspice + "' -b case.cir > ngspice.log 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        error = "ngspice failed (" + std::to_string(rc) + "); see " + work + "/ngspice.log";
        return false;
    }
    return true;
}

inline std::string ngspice_version(const std::string& ngspice, const std::string& work) {
    const std::string log = work + "/version.txt";
    const std::string cmd = "'" + ngspice + "' --version > '" + log + "' 2>&1";
    if (std::system(cmd.c_str()) != 0) return "unknown";
    std::string text;
    read_file(log, text);
    const auto at = text.find("ngspice-");
    if (at == std::string::npos) return "unknown";
    const auto end = text.find_first_of(" \n:", at);
    return text.substr(at, end - at);
}

}  // namespace sonora::measure::spice_util
