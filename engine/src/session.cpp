// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session.h"

#include <sys/stat.h>

#include <cstdio>
#include <string>
#include <vector>

namespace sonora {
namespace {

bool make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0775) == 0) return true;
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string audio_dir(const std::string& session_dir) {
    return session_dir + "/Audio Files";
}

}  // namespace

Session::Session(SessionConfig cfg) : cfg_(std::move(cfg)) {
    transport_ = std::make_unique<Transport>(cfg_.sample_rate);

    const auto ring_frames =
        static_cast<std::size_t>(cfg_.ring_seconds * cfg_.sample_rate);

    std::vector<Track*> raw;
    for (int i = 0; i < cfg_.track_count; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Audio %d", i + 1);
        tracks_.push_back(std::make_unique<Track>(i, name,
                                                  cfg_.channels_per_track,
                                                  ring_frames));
        raw.push_back(tracks_.back().get());
    }

    // 64 kframes per disk write keeps syscall count low without stalling.
    disk_ = std::make_unique<DiskThread>(std::move(raw), 65536);
}

Session::~Session() { stop(); }

bool Session::create_dirs() {
    if (!make_dir(cfg_.dir)) return false;
    return make_dir(audio_dir(cfg_.dir));
}

bool Session::start_recording(int take_number) {
    const uint64_t origin = transport_->playhead();
    bool any = false;

    for (auto& t : tracks_) {
        if (!t->rec_armed()) continue;
        char file[512];
        std::snprintf(file, sizeof(file), "%s/%s_%02d.wav",
                      audio_dir(cfg_.dir).c_str(), t->name().c_str(),
                      take_number);
        t->capture_ring().reset();
        if (!t->writer().open(file, cfg_.sample_rate, t->channels(),
                              cfg_.format, origin)) {
            std::fprintf(stderr, "sonora: cannot open %s\n", file);
            continue;
        }
        any = true;
    }

    if (!any) return false;
    transport_->set_state(TransportState::Recording);
    return true;
}

void Session::stop() {
    transport_->set_state(TransportState::Stopped);
    // Let the disk thread flush what is still buffered before we close files.
    if (disk_) disk_->stop();
    for (auto& t : tracks_) t->writer().close();
}

void Session::process(const float* const* inputs, uint32_t nframes) {
    const bool recording = transport_->state() == TransportState::Recording;

    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        Track* t = tracks_[i].get();
        const float* in = inputs[i];
        if (!in) continue;

        // Meter every input, armed or not, so the UI shows signal before
        // anyone hits record.
        float peak = 0.0f;
        for (uint32_t f = 0; f < nframes; ++f) {
            const float a = in[f] < 0.0f ? -in[f] : in[f];
            if (a > peak) peak = a;
        }
        t->report_peak(peak);

        if (!recording || !t->rec_armed()) continue;

        // Capture is pre-fader by design: the fader is a monitoring decision
        // and must not be baked into the recorded file.
        const std::size_t wrote = t->capture_ring().write(in, nframes);
        if (wrote < nframes) t->note_overrun();
    }

    if (transport_->state() != TransportState::Stopped) {
        transport_->advance(nframes);
    }
}

}  // namespace sonora
