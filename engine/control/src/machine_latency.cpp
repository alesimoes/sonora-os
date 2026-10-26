// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/machine_latency.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <vector>

#include "sonora/platform/file.h"
#include "sonora/platform/process.h"

namespace sonora {

namespace fs = std::filesystem;

namespace {

// A file far over what a machine's interfaces could fill is not ours.
constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{1} << 20;

const char* source_word(api::LatencySource s) {
    return s == api::LatencySource::Manual ? "manual" : "measured";
}

// One line: rate, period, frames, source and the device last, so a device
// name may hold spaces.
struct Line {
    LatencyKey key;
    KeptLatency kept;
};

bool parse_line(const std::string& text, Line& out) {
    std::vector<std::string> fields;
    std::size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t tab = text.find('\t', at);
        if (tab == std::string::npos) return false;
        fields.push_back(text.substr(at, tab - at));
        at = tab + 1;
    }
    out.key.device = text.substr(at);
    const auto number = [](const std::string& s, uint32_t& v) {
        if (s.empty() || s.size() > 9) return false;
        uint32_t n = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + static_cast<uint32_t>(c - '0');
        }
        v = n;
        return true;
    };
    if (!number(fields[0], out.key.rate) || !number(fields[1], out.key.period) ||
        !number(fields[2], out.kept.frames)) {
        return false;
    }
    if (fields[3] == "measured") {
        out.kept.source = api::LatencySource::Measured;
    } else if (fields[3] == "manual") {
        out.kept.source = api::LatencySource::Manual;
    } else {
        return false;
    }
    return true;
}

bool same_key(const LatencyKey& a, const LatencyKey& b) {
    return a.device == b.device && a.rate == b.rate && a.period == b.period;
}

std::vector<Line> read_lines(const std::string& path) {
    std::vector<Line> lines;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxFileBytes) return lines;
    std::ifstream in(path, std::ios::binary);
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty() || text[0] == '#') continue;
        Line l;
        if (parse_line(text, l)) lines.push_back(std::move(l));
    }
    return lines;
}

}  // namespace

std::string machine_latency_file(const std::string& config_dir) {
    if (config_dir.empty()) return {};
    return (fs::path(config_dir) / "latency.conf").string();
}

bool load_machine_latency(const std::string& path, const LatencyKey& key, KeptLatency& out) {
    if (path.empty()) return false;
    for (const Line& l : read_lines(path)) {
        if (same_key(l.key, key)) {
            out = l.kept;
            return true;
        }
    }
    return false;
}

bool save_machine_latency(const std::string& path, const LatencyKey& key, const KeptLatency& kept,
                          std::string& why) {
    if (path.empty()) {
        why = "there is no config folder to keep the round trip in";
        return false;
    }
    if (key.device.find_first_of("\t\r\n") != std::string::npos) {
        why = "the capture device's name cannot be kept in latency.conf";
        return false;
    }
    if (kept.source != api::LatencySource::Measured && kept.source != api::LatencySource::Manual) {
        why = "only a measured or a manual round trip is kept";
        return false;
    }
    std::vector<Line> lines = read_lines(path);
    bool replaced = false;
    for (Line& l : lines) {
        if (same_key(l.key, key)) {
            l.kept = kept;
            replaced = true;
        }
    }
    if (!replaced) lines.push_back(Line{key, kept});
    std::ostringstream text;
    text << "# SonoraOS: the round trip in frames per capture device, rate and period\n"
            "# (ADR-0019). rate<TAB>period<TAB>frames<TAB>source<TAB>device\n";
    for (const Line& l : lines) {
        text << l.key.rate << '\t' << l.key.period << '\t' << l.kept.frames << '\t'
             << source_word(l.kept.source) << '\t' << l.key.device << '\n';
    }
    const std::string bytes = text.str();

    const fs::path target(path);
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string tmp = (dir / (target.filename().string() + ".tmp-" +
                                    std::to_string(platform::current_process_id()) + "-" +
                                    std::to_string(std::random_device{}())))
                                .string();
    std::FILE* f = platform::create_exclusive(tmp);
    if (!f) {
        why = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool synced = written && platform::sync_data(f);
    const bool closed = std::fclose(f) == 0;
    if (!written || !synced || !closed) {
        why = "cannot write " + tmp + ": " + std::strerror(errno);
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        why = "cannot replace " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    // A setting, not a take: a lost rename costs a measurement, so the
    // folder's sync failing is not worth refusing what was written.
    platform::sync_directory(dir.string());
    return true;
}

}  // namespace sonora
