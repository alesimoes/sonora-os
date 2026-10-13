// SPDX-License-Identifier: GPL-3.0-or-later
//
// The measurement harness (ADR-0011 §3.1): renders a processor offline
// through the Processor interface, measures it against its bars in
// plugins/native/<name>/quality.json, compares golden files, and writes one
// JSON report per processor.
//
// Inputs are generated, never recorded, from fixed seeds. Nothing here runs
// on an audio thread; process() under test does, and the harness is what
// executes it for the RTSan job.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sonora/json/json.h"
#include "sonora/native/processor.h"

namespace sonora::measure {

using Chans = std::vector<std::vector<float>>;
using Factory = std::function<std::unique_ptr<native::Processor>()>;

// ---- signals -------------------------------------------------------------

Chans sine(double fs, double freq, double dbfs, double seconds, std::uint32_t channels = 2);
Chans silence(double fs, double seconds, std::uint32_t channels = 2);
// Uniform white noise from xorshift32, per channel seeds seed + c.
Chans noise(double fs, double dbfs_peak, double seconds, std::uint32_t seed, std::uint32_t channels = 2);
// Exponential sweep (A. Farina, AES 108th Convention, 2000).
Chans log_sweep(double fs, double f1, double f2, double dbfs, double seconds,
                std::uint32_t channels = 2);
// Low-passes at f_hz with a Kaiser-windowed sinc and fades 10 ms in and out.
void band_limit_and_fade(Chans& x, double fs, double f_hz = 20000.0);
Chans concat(const std::vector<Chans>& parts);
// The fixed golden input: noise burst, sweep, impulse, decay; `seconds` long.
Chans golden_input(double fs, double seconds);

// ---- rendering -------------------------------------------------------------

// An automation point: at absolute frame `frame`, set param `index` to value.
struct Event {
    std::uint64_t frame;
    std::uint32_t index;
    float value;
};

struct RenderOptions {
    std::uint32_t block = 128;
    native::Transport transport;
    const Chans* sidechain = nullptr;
    // Per-block wall time in microseconds, if non-null.
    std::vector<double>* block_us = nullptr;
};

Chans render(native::Processor& p, const Chans& in, const std::vector<Event>& events = {},
             const RenderOptions& opt = {});

// Finds a parameter's index by id; aborts the run if it does not exist,
// since a suite naming a missing id is a broken suite.
std::uint32_t param_index(const native::Processor& p, const char* id);
void set(native::Processor& p, const char* id, float value);

// ---- analysis --------------------------------------------------------------

double peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX);
double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX);
double to_db(double linear);
// Response at f of an impulse response (direct DFT), in dB and degrees,
// with `delay` samples of pure delay removed from the phase.
std::pair<double, double> response_db_deg(const std::vector<float>& ir, double f, double fs,
                                          double delay = 0.0);
// THD+N of a steady sine at f0 in y[from, from + 32768): everything but the
// fundamental, relative to it, from 20 Hz to 20 kHz.
double thdn_db(const std::vector<float>& y, std::size_t from, double fs, double f0);
// Non-harmonic energy relative to the fundamental: what aliasing puts in
// the band.
double alias_dbc(const std::vector<float>& y, std::size_t from, double fs, double f0);
// Lag in [0, max_lag] maximising the cross-correlation of out with in.
long xcorr_lag(const std::vector<float>& in, const std::vector<float>& out, long max_lag);
// Peak of y high-passed at 5 kHz (8th-order Butterworth, matched): a step
// or a click shows up here, a smooth change of a 100 Hz tone does not.
double hf_residue_peak(const std::vector<float>& y, double fs, std::size_t from = 0);
// 16x true peak, Kaiser-windowed sinc +-256 taps (finer, and longer, than
// any detector of ours), evaluated between samples where either neighbour
// reaches near_level; elsewhere a reconstruction cannot reach the ceiling.
double reference_true_peak(const std::vector<float>& y, double near_level);

// ---- golden files ------------------------------------------------------------

bool write_wav_f32(const std::string& path, const Chans& x, double fs);
bool read_wav_f32(const std::string& path, Chans& x, double& fs, std::string& error);

// ---- the suite -----------------------------------------------------------

struct Args {
    std::string quality_path;  // plugins/native/<name>/quality.json
    std::string golden_dir;    // plugins/native/<name>/tests/golden
    std::string report_dir;    // build/dsp-reports; empty: none
    bool regenerate_golden = false;
    bool cpu = false;          // the 60 s CPU measurement (not run in ctest)
    double cpu_seconds = 60.0;
};

class Suite {
public:
    Suite(std::string name, Factory factory, Args args);

    const std::string& name() const { return name_; }
    std::unique_ptr<native::Processor> make() const { return factory_(); }
    const Args& args() const { return args_; }

    // A bar from quality.json, by dotted path ("eq.magnitude_db"). A
    // missing bar fails the suite: a bar cannot vanish silently.
    double bar(const std::string& path);
    const json::Value* quality(const std::string& path) const;

    // Records a measured value against its bar.
    void check(const std::string& what, double value, double bar, bool pass,
               const std::string& unit, const std::string& note = {});
    void check_le(const std::string& what, double value, double bar, const std::string& unit,
                  const std::string& note = {}) {
        check(what, value, bar, value <= bar, unit, note);
    }
    void info(const std::string& what, double value, const std::string& unit,
              const std::string& note = {});
    void fail(const std::string& why);
    // Free-form data for the report (curves for the plots).
    json::Value& data() { return data_; }

    // Prints the table, writes the report; returns the process exit code.
    int finish();

private:
    std::string name_;
    Factory factory_;
    Args args_;
    json::Value quality_;
    json::Value checks_ = json::Value::array();
    json::Value data_ = json::Value::object();
    int failures_ = 0;
};

// ---- the checks every processor runs ----------------------------------------

const double kRates[] = {44100.0, 48000.0, 96000.0};

using Config = std::vector<std::pair<const char*, float>>;
void apply(native::Processor& p, const Config& c);

// Latency measured equals latency reported, in every configuration and at
// every rate; bypass is the input delayed by exactly that latency.
void check_latency(Suite& s, const std::vector<std::pair<std::string, Config>>& configs);
// NaN and Inf in, finite out; identical to the run with silence instead,
// so recovery is immediate.
void check_nonfinite(Suite& s, const Config& c);
// Per-block CPU after 10 s of silence following signal, against active.
void check_denormals(Suite& s, const Config& c);
// Full-range sweeps of each automatable parameter listed in quality.json
// "zipper", delivered once per 128-frame block as a host would.
// `label` tells passes over different configurations apart in the report.
void check_zipper(Suite& s, const Config& c, const std::string& label = "");
// The zipper checks' input: a 100 Hz tone at -6 dBFS, faded in.
Chans zipper_tone(double fs, double seconds);
// Golden renders at 44.1, 48 and 96 kHz against tests/golden.
void check_golden(Suite& s, const Config& c);
// p50, p99.9 and max per block at q128, 48 kHz, stereo (--cpu only).
void check_cpu(Suite& s, const Config& c, const std::string& label = "");
// Runs all of the above. The zipper sweeps run on `zipper` when given (a
// configuration where every swept parameter is audible), else on golden.
void check_common(Suite& s, const Config& golden,
                  const std::vector<std::pair<std::string, Config>>& latency_configs,
                  const Config* zipper = nullptr);

}  // namespace sonora::measure
