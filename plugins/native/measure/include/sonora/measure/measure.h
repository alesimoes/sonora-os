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

#include "sonora/dsp/biquad.h"
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
// The octave-band filter centred on band_hz: a sixth-order Butterworth
// band-pass (a third-order prototype), -3 dB at fc 2^(+-1/2), unity at fc,
// as biquad sections (analysis.cpp gives its response). Returns how many
// it wrote (at most 5); a third-order high-pass at the lower edge when the
// upper edge is above 0.49 fs.
int octave_band_sections(double fs, double band_hz, dsp::BiquadCoeffs* out);
// x filtered to the octave band centred on band_hz (octave_band_sections;
// band_hz 0: unfiltered), optionally time-reversed first.
std::vector<double> octave_band(const std::vector<float>& x, double fs, double band_hz, bool reversed = false);
// Reverberation time from an impulse response, ISO 3382-1:2009: filtered
// to the octave band (band_hz 0: broadband), time-reversed so the filter's
// ringing does not lengthen the decay, Schroeder backward integration, and
// a least-squares line through the decay curve from -5 to -35 dB (T30),
// extrapolated to 60 dB. 0 when the curve never reaches -35 dB. The curve
// in dB is appended to edc_db when given.
double t30_seconds(const std::vector<float>& ir, double fs, double band_hz,
                   std::vector<double>* edc_db = nullptr);
// The same from several responses of one system (sources and receivers),
// their band energies summed before the integration, as ISO 3382-1
// averages decay curves over positions. Equal lengths.
double t30_seconds(const Chans& irs, double fs, double band_hz, std::vector<double>* edc_db = nullptr);
// 16x true peak, Kaiser-windowed sinc +-256 taps (finer, and longer, than
// any detector of ours), exact wherever it reaches near_level
// (common/dsp/reference_peak.h says how it finds those places).
double reference_true_peak(const std::vector<float>& y, double near_level);
// What a BS.1770-5 Annex 2 meter reads: common/dsp's meter detector, 4x
// and 32 taps per phase, as loudness.h runs it.
double meter_true_peak(const std::vector<float>& y);

// SONORA_DSP_QUICK=1: the slow sweeps (the limiter's stress set) run one
// rate and one setting instead of all of them. PR CI and the sanitizer
// builds set it; the nightly workflow does not.
bool quick();

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
    // Hold the CPU bars scaled by this machine's speed against the
    // reference machine's, measured with the reference kernel
    // (calibrate.cpp): for a shared CI runner. Off, the bars are absolute.
    bool cpu_calibrate = false;
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

    // --cpu-calibrate (calibrate.cpp). start_cpu_calibration() times the
    // reference kernel once, before the first load; calibrated_cpu()
    // records a load's p99.9 and max ("what" ends in ": "). finish() times
    // the kernel again and, when the two timings agree within
    // kCalibrationDrift, holds each load to the class bars scaled by the
    // kernel's cost here over the reference machine's, and its p99.9 in
    // kernels to cpu.p999_kernels x kKernelRegression when quality.json
    // records that baseline. When they disagree the machine changed speed
    // during the run: the checks are inconclusive, neither passed nor
    // failed, and finish() returns kInconclusive.
    void start_cpu_calibration();
    void calibrated_cpu(const std::string& what, double p999_us, double max_us);
    static constexpr double kCalibrationDrift = 0.05;
    static constexpr double kKernelRegression = 1.15;
    static constexpr int kInconclusive = 3;

    // Prints the table, writes the report; returns the process exit code:
    // 0, 1 when a check failed, or kInconclusive.
    int finish();

private:
    std::string name_;
    Factory factory_;
    Args args_;
    json::Value quality_;
    json::Value checks_ = json::Value::array();
    json::Value data_ = json::Value::object();
    int failures_ = 0;
    struct CalibratedCpu {
        std::string what;
        double p999_us, max_us;
    };
    std::vector<CalibratedCpu> calibrated_;
    double kernel_before_us_ = 0.0;
    bool inconclusive_ = false;
    void finish_cpu_calibration();
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
// reset() from a realtime context (where ProcessorBase calls on_reset when
// the output goes non-finite), then the same render as a fresh processor.
void check_reset(Suite& s, const Config& c);
// Per-block CPU after 10 s of silence following signal, against active.
void check_denormals(Suite& s, const Config& c);
// Full-range sweeps of each automatable parameter listed in quality.json
// "zipper", delivered once per 128-frame block as a host would.
// `label` tells passes over different configurations apart in the report.
void check_zipper(Suite& s, const Config& c, const std::string& label = "");
// The zipper checks' input: a 100 Hz tone, -6 dBFS unless quality.json's
// "zipper_tone_dbfs" says otherwise (the bar then moves with it), faded in.
Chans zipper_tone(double fs, double seconds, double dbfs = -6.0);
// Golden renders at 44.1, 48 and 96 kHz against tests/golden.
void check_golden(Suite& s, const Config& c);
// p50, p99.9 and max per block at q128, 48 kHz, stereo (--cpu only).
// assert_bar false: the figures are reported, not held to the class bar (a
// quality over its class beside an eco mode that is held to it, ADR-0011
// §3.5).
void check_cpu(Suite& s, const Config& c, const std::string& label = "", bool assert_bar = true);
// The same on a given 48 kHz stereo input (a worst case: a hot input at
// full drive), at least a few seconds long.
void check_cpu_on(Suite& s, const Config& c, const Chans& in, const std::string& label, bool assert_bar = true);
// check_cpu's programme signal (noise with a log sweep under it, peaks
// about -12 dBFS), raised by gain_db.
Chans cpu_programme(double fs, double seconds, double gain_db);
// The reference kernel's p50 per q128 stereo block on this machine
// (per-block minimum of 5 runs over `seconds`), and on the reference
// machine: their ratio scales the CPU bars under --cpu-calibrate.
double reference_kernel_us(double seconds);
double reference_kernel_reference_us();
// Runs all of the above. The zipper sweeps run on `zipper` when given (a
// configuration where every swept parameter is audible), else on golden.
void check_common(Suite& s, const Config& golden,
                  const std::vector<std::pair<std::string, Config>>& latency_configs,
                  const Config* zipper = nullptr);

}  // namespace sonora::measure
