// SPDX-License-Identifier: GPL-3.0-or-later
// One-shot factory preset reads (factory_preset_reader.h, ADR-0029 §5).
#include "sonora/factory_preset_reader.h"

#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

#include "sonora/platform/process.h"
#include "sonora/plugin/host_control.h"

namespace sonora {

namespace pr = plugin::probe;
namespace pc = plugin::control;

static_assert(pr::kMaxPresetStateBytes == pc::kMaxStateBytes, "a preset's state is a StateBlob");

namespace {

// What a well-behaved host writes at most: the line's cap, its newline and
// the blob's. Anything longer is not read into memory.
constexpr std::size_t kMaxAnswerBytes = pr::kMaxRecordBytes + 1 + pr::kMaxPresetStateBytes;

bool read_answer(const std::string& path, std::string& out, std::string& why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        why = "no answer";
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxAnswerBytes) {
        why = "an answer of " + std::to_string(size) + " bytes, over the cap of " + std::to_string(kMaxAnswerBytes);
        return false;
    }
    f.seekg(0);
    out.assign(static_cast<std::size_t>(size), '\0');
    f.read(out.data(), size);
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(f.gcount(), 0)));
    return true;
}

std::string seconds(uint64_t ms) {
    return ms % 1000 == 0 ? std::to_string(ms / 1000) + " s" : std::to_string(ms) + " ms";
}

}  // namespace

FactoryPresetReader::FactoryPresetReader(FactoryPresetReaderConfig config) : config_(std::move(config)) {}

pr::PresetRead FactoryPresetReader::blocked(const std::string& plugin, const std::string& preset, std::string why,
                                            bool remember) {
    if (remember) blocked_[{plugin, preset}] = why;
    pr::PresetRead r;
    r.plugin = plugin;
    r.preset = preset;
    r.outcome = pr::Outcome::Blocked;
    r.reason = std::move(why);
    return r;
}

pr::PresetRead FactoryPresetReader::read(const std::string& bundle, const std::string& plugin,
                                         const std::string& preset) {
    if (const auto it = blocked_.find({plugin, preset}); it != blocked_.end())
        return blocked(plugin, preset, it->second + "; not read again until the plug-ins are rescanned", false);

    const std::string out_path = config_.scratch_dir + "/sonora-preset-" +
                                 std::to_string(platform::current_process_id()) + "-" +
                                 std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()) +
                                 ".out";
    std::vector<std::string> argv = {config_.host_path, "--read-preset", bundle, preset, "--plugin", plugin};
    for (const std::string& b : config_.bundles) {
        argv.push_back("--bundle");
        argv.push_back(b);
    }
    argv.push_back("--engine-pid");
    argv.push_back(std::to_string(platform::current_process_id()));
    platform::ChildProcess::Options o;
    o.stdout_path = out_path;
    o.nice = config_.nice;
    o.idle_io = config_.idle_io;
    // A Ctrl+C or a signal to the engine's group is not the reader's.
    o.own_group = true;
    std::string error;
    auto child = platform::ChildProcess::spawn(argv, o, error);
    if (!child) {
        std::remove(out_path.c_str());
        return blocked(plugin, preset, "its preset reader could not start: " + error, false);
    }
    ++children_;
    const auto timeout = config_.timeout_ms > uint64_t(INT32_MAX) ? INT32_MAX : int(config_.timeout_ms);
    if (!child->wait(timeout)) {
        child->kill();
        child->wait(-1);
        std::remove(out_path.c_str());
        return blocked(plugin, preset, "its preset reader did not answer within " + seconds(config_.timeout_ms),
                       true);
    }
    std::string answer, why;
    const bool have = read_answer(out_path, answer, why);
    std::remove(out_path.c_str());
    if (!child->exited_normally()) {
        const int sig = child->signal_number();
        const bool ours = sig == SIGINT || sig == SIGTERM || sig == SIGHUP || sig == SIGQUIT;
        return blocked(plugin, preset, "its preset reader was killed by signal " + std::to_string(sig), !ours);
    }
    pr::PresetRead r;
    if (!have || !pr::decode_preset_read(answer, r, why))
        return blocked(plugin, preset,
                       "its preset reader exited with code " + std::to_string(child->exit_code()) +
                           " without a preset (" + why + ")",
                       true);
    if (r.plugin != plugin || r.preset != preset)
        return blocked(plugin, preset, "its preset reader answered for " + r.preset + " of " + r.plugin, true);
    if (r.outcome == pr::Outcome::Blocked)
        return blocked(plugin, preset, "its preset reader answered \"blocked\", which only the engine says", true);
    if (r.ok() && !r.state.empty()) {
        // The host checks this too; a session must never hold another
        // plugin's state under this one's name, so it is checked again here.
        pc::StateBlob b;
        const bool vst3 = plugin.rfind("vst3:", 0) == 0;
        if (!pc::decode_state_blob(r.state, b, why))
            return blocked(plugin, preset, "its preset reader wrote a state that does not decode (" + why + ")", true);
        if (b.format != (vst3 ? pc::Format::Vst3 : pc::Format::Lv2) || b.id != (vst3 ? plugin.substr(5) : plugin))
            return blocked(plugin, preset, "its preset reader wrote " + b.id + "'s state", true);
    }
    return r;
}

}  // namespace sonora
