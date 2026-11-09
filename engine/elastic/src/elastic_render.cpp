// SPDX-License-Identifier: GPL-3.0-or-later
//
// render_elastic(): the worker's whole job, callable without a thread so the
// tests drive it alone.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <utility>

#include "sonora/elastic/elastic_job.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/stretch/quantize.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

namespace sonora::elastic {

namespace {

constexpr model::Frames kBlock = 65536;
// Progress, in thousandths, at the end of each stage: read, stretch, write.
constexpr std::uint32_t kRead = 300, kStretched = 800, kWritten = 1000;
constexpr const char* kCancelled = "the stretch was cancelled";

std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

stretch::Algorithm algorithm_of(api::ElasticAlgorithm a) {
    switch (a) {
        case api::ElasticAlgorithm::Polyphonic: return stretch::Algorithm::Polyphonic;
        case api::ElasticAlgorithm::Rhythmic: return stretch::Algorithm::Rhythmic;
        case api::ElasticAlgorithm::Monophonic: return stretch::Algorithm::Monophonic;
        case api::ElasticAlgorithm::Varispeed: return stretch::Algorithm::Varispeed;
    }
    return stretch::Algorithm::Polyphonic;
}

void set_progress(std::atomic<std::uint32_t>& permille, std::uint32_t from, std::uint32_t to,
                  model::Frames done, model::Frames total) {
    const double f = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0;
    permille.store(from + static_cast<std::uint32_t>(f * (to - from)), std::memory_order_relaxed);
}

// Every region channel's span and handles, each file read once.
bool read_input(const ElasticPlan& plan, const std::string& session_dir,
                const std::atomic<bool>& cancel, std::atomic<std::uint32_t>& permille,
                stretch::Planar& in, std::string& why) {
    const model::Frames total = plan.lead + plan.region.length + plan.tail;
    in.assign(plan.files.size(), std::vector<float>(static_cast<std::size_t>(total)));
    std::map<std::string, std::vector<std::size_t>> by_file;
    for (std::size_t c = 0; c < plan.files.size(); ++c) by_file[plan.files[c].path].push_back(c);
    std::vector<float> raw;
    model::Frames done = 0;
    const model::Frames all = total * static_cast<model::Frames>(by_file.size());
    for (const auto& [path, channels] : by_file) {
        WavReader rd;
        if (!rd.open(session_dir + "/" + path, why)) {
            why = path + ": " + why;
            return false;
        }
        if (rd.sample_rate() != plan.sample_rate) {
            why = path + " is at " + std::to_string(rd.sample_rate()) +
                  " Hz, and the session runs at " + std::to_string(plan.sample_rate) + " Hz";
            return false;
        }
        for (const std::size_t c : channels) {
            if (plan.files[c].channel >= rd.channels()) {
                why = path + " has no channel " + std::to_string(plan.files[c].channel + 1);
                return false;
            }
        }
        for (model::Frames b0 = 0; b0 < total; b0 += kBlock) {
            if (cancel.load(std::memory_order_relaxed)) {
                why = kCancelled;
                return false;
            }
            const model::Frames n = std::min(kBlock, total - b0);
            raw.resize(static_cast<std::size_t>(n) * rd.channels());
            if (rd.read(plan.s0 + b0, n, raw.data()) != n) {
                why = path + " cannot be read where the region is";
                return false;
            }
            for (const std::size_t c : channels) {
                const std::uint16_t ch = plan.files[c].channel;
                for (model::Frames i = 0; i < n; ++i) {
                    in[c][static_cast<std::size_t>(b0 + i)] =
                        raw[static_cast<std::size_t>(i) * rd.channels() + ch];
                }
            }
            done += n;
            set_progress(permille, 0, kRead, done, all);
        }
    }
    return true;
}

// Quantize's markers from the region's onsets and the tempo map.
bool quantize_markers(const ElasticPlan& plan, const stretch::Planar& in, OnsetSource* onsets,
                      std::vector<stretch::Marker>& out, std::string& why) {
    if (onsets == nullptr) {
        why = "this engine has no onset detector for Quantize Audio";
        return false;
    }
    stretch::QuantizeRequest q;
    if (!onsets->find(in, plan.lead, plan.region.length, plan.sample_rate, q.onsets, why)) {
        return false;
    }
    q.region_start = plan.region.position;
    q.region_length = plan.region.length;
    q.grid_beats = plan.request.grid_beats;
    q.strength = plan.request.strength;
    stretch::WarpMap map;
    if (!stretch::quantize_plan(q, plan.tempo, map, why)) {
        why = "Quantize Audio found nothing to move: " + why;
        return false;
    }
    if (map.output_length() > api::kMaxElasticFrames) {
        why = "the quantized region would be over the " + std::to_string(api::kMaxElasticFrames) +
              " frames an elastic render holds in memory";
        return false;
    }
    if (!fits_before_next(plan, map.output_length(), why)) return false;
    out = map.markers();
    return true;
}

// Opens "<track>_elastic_NN.wav" with the first free NN: open() refuses an
// existing name with EEXIST and leaves that file untouched.
bool open_copy(const ElasticPlan& plan, const std::string& dir, std::uint16_t channels,
               WavWriter& writer, std::string& file, std::string& why) {
    const WavWriter::Format format = plan.format == model::AudioFormat::Float32
                                         ? WavWriter::Format::Float32
                                         : WavWriter::Format::Int24;
    writer.set_description(std::string("Elastic Audio: ") +
                           api::elastic_algorithm_name(plan.request.algorithm));
    for (int n = 1; n <= 9999; ++n) {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%02d", n);
        file = file_safe(plan.track_name) + "_elastic_" + digits + ".wav";
        if (writer.open(dir + "/" + file, plan.sample_rate, channels, format,
                        static_cast<std::uint64_t>(plan.time_reference))) {
            return true;
        }
        if (errno != EEXIST) {
            why = "cannot create " + file + ": " + std::strerror(errno);
            return false;
        }
    }
    why = "no free elastic file name for " + plan.track_name;
    return false;
}

bool write_copy(const ElasticPlan& plan, const stretch::Planar& audio, WavWriter& writer,
                const std::atomic<bool>& cancel, std::atomic<std::uint32_t>& permille,
                std::string& why) {
    const std::size_t nch = audio.size();
    const auto total = static_cast<model::Frames>(audio[0].size());
    const bool clamp = plan.format != model::AudioFormat::Float32;
    std::vector<float> mix(static_cast<std::size_t>(kBlock) * nch);
    for (model::Frames b0 = 0; b0 < total; b0 += kBlock) {
        if (cancel.load(std::memory_order_relaxed)) {
            why = kCancelled;
            return false;
        }
        const model::Frames n = std::min(kBlock, total - b0);
        for (model::Frames i = 0; i < n; ++i) {
            for (std::size_t c = 0; c < nch; ++c) {
                float v = audio[c][static_cast<std::size_t>(b0 + i)];
                if (clamp) v = std::clamp(v, -1.0f, 1.0f);
                mix[static_cast<std::size_t>(i) * nch + c] = v;
            }
        }
        if (!writer.write(mix.data(), static_cast<std::size_t>(n))) {
            why = writer.failure();
            return false;
        }
        set_progress(permille, kStretched, kWritten - 1, b0 + n, total);
    }
    // Synced, with its folder, before the document may name it.
    if (!writer.checkpoint() || !writer.close()) {
        why = writer.failure().empty() ? "the disk refused it" : writer.failure();
        return false;
    }
    return true;
}

// The numbers come from the file, as for every source.
bool describe_copy(const std::string& full, const std::string& file, model::Source& out,
                   std::string& why) {
    WavReader written;
    if (!written.open(full, why)) return false;
    out.path = std::string(session_files::kAudioDir) + "/" + file;
    out.channels = written.channels();
    out.length = written.frames();
    out.format = written.encoding() == WavReader::Encoding::Float32 ? model::AudioFormat::Float32
                                                                     : model::AudioFormat::Int24;
    out.time_reference = static_cast<std::int64_t>(written.time_reference());
    out.kind = model::SourceKind::Imported;
    return true;
}

}  // namespace

bool render_elastic(const ElasticPlan& plan, const std::string& session_dir,
                    const StretcherFactory& make_stretcher, OnsetSource* onsets,
                    const std::atomic<bool>& cancel, std::atomic<std::uint32_t>& permille,
                    ElasticOutput& out, std::string& why) {
    permille.store(0, std::memory_order_relaxed);
    stretch::Planar audio;
    if (!read_input(plan, session_dir, cancel, permille, audio, why)) return false;
    std::vector<stretch::Marker> markers = plan.markers;
    if (plan.request.mode == api::ElasticMode::Quantize &&
        !quantize_markers(plan, audio, onsets, markers, why)) {
        return false;
    }
    stretch::WarpMap map;
    if (!input_map(plan, markers, map, why)) return false;
    std::unique_ptr<stretch::Stretcher> stretcher =
        make_stretcher ? make_stretcher(plan.sample_rate, cancel) : nullptr;
    if (!stretcher) {
        why = "this engine was built without a stretcher";
        return false;
    }
    stretch::Planar stretched;
    if (!stretcher->process(audio, map, algorithm_of(plan.request.algorithm), stretched, why)) {
        if (cancel.load(std::memory_order_relaxed)) why = kCancelled;
        return false;
    }
    if (stretched.size() != audio.size() ||
        static_cast<model::Frames>(stretched[0].size()) != map.output_length()) {
        why = "the stretcher returned " + std::to_string(stretched[0].size()) +
              " frames where " + std::to_string(map.output_length()) + " were asked for";
        return false;
    }
    stretch::Planar().swap(audio);
    permille.store(kStretched, std::memory_order_relaxed);

    const std::string dir = session_dir + "/" + session_files::kAudioDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        why = "cannot create " + dir;
        return false;
    }
    WavWriter writer;
    std::string file;
    if (!open_copy(plan, dir, static_cast<std::uint16_t>(stretched.size()), writer, file, why)) {
        return false;
    }
    const std::string full = dir + "/" + file;
    // A partial copy is never left behind: cancelled, failed or unreadable.
    if (!write_copy(plan, stretched, writer, cancel, permille, why) ||
        !describe_copy(full, file, out.source, why)) {
        writer.close();
        std::remove(full.c_str());
        if (why != kCancelled) why = "cannot write " + file + ": " + why;
        return false;
    }
    out.markers = markers;
    out.region_length = markers.empty()
                            ? plan.region.length
                            : markers.back().target + (plan.region.length - markers.back().source);
    permille.store(kWritten, std::memory_order_relaxed);
    return true;
}

}  // namespace sonora::elastic
