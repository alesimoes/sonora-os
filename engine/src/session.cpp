// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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

// The take number in a name start_recording() writes, "<track>_<take>.wav",
// or 0 if the name is not of that shape.
int take_of(const char* name) {
    const std::size_t len = std::strlen(name);
    const std::size_t ext = 4;  // ".wav"
    if (len <= ext || std::strcmp(name + len - ext, ".wav") != 0) return 0;
    std::size_t start = len - ext;
    while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') --start;
    const std::size_t digits = len - ext - start;
    // Nine digits fit an int; a longer run is not a take number we wrote.
    if (digits == 0 || digits > 9 || start == 0 || name[start - 1] != '_') return 0;
    int n = 0;
    for (std::size_t i = start; i < len - ext; ++i) n = n * 10 + (name[i] - '0');
    return n;
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

int Session::next_free_take() const {
    // Every track shares the take number, so any file of ours counts, on any
    // track, armed now or not.
    int highest = 0;
    DIR* d = ::opendir(audio_dir(cfg_.dir).c_str());
    if (!d) return 1;
    while (const dirent* e = ::readdir(d)) {
        const int n = take_of(e->d_name);
        if (n > highest) highest = n;
    }
    ::closedir(d);
    return highest + 1;
}

bool Session::start_recording(int take_number, std::string* error) {
    const uint64_t origin = transport_->playhead();
    std::vector<Track*> opened;

    for (auto& t : tracks_) {
        if (!t->rec_armed()) continue;
        char file[512];
        std::snprintf(file, sizeof(file), "%s/%s_%02d.wav",
                      audio_dir(cfg_.dir).c_str(), t->name().c_str(),
                      take_number);
        // Neither other thread touches this ring now: the audio thread
        // writes it only once the transport says Recording, the disk thread
        // reads it only once the track is published below. Both of those are
        // release stores that come after this, which is what makes these
        // relaxed index stores safe.
        t->capture_ring().reset();
        if (!t->writer().open(file, cfg_.sample_rate, t->channels(),
                              cfg_.format, origin)) {
            const int err = errno;
            // A take missing a track is not the take that was armed, and the
            // operator would find out at playback. Undo and say so instead.
            // Removing is safe: the writer creates exclusively, so every file
            // in `opened` was made by this call and holds nothing yet.
            for (Track* o : opened) {
                const std::string path = o->writer().path();
                o->writer().close();
                std::remove(path.c_str());
            }
            const std::string why =
                std::string(file) + ": " +
                (err == EEXIST ? "a take with this name is already on disk"
                               : std::strerror(err));
            std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
            if (error) *error = why;
            return false;
        }
        opened.push_back(t.get());
    }

    if (opened.empty()) {
        if (error) *error = "no track is armed";
        return false;
    }
    // Only now may the disk thread see these writers; until here the
    // rollback above could close them under it. See Track::set_draining().
    for (Track* t : opened) t->set_draining(true);
    transport_->set_state(TransportState::Recording);
    return true;
}

void Session::stop() {
    transport_->set_state(TransportState::Stopped);
    // Let the disk thread flush what is still buffered before we close files.
    // stop() joins it, so after this line nothing else uses the writers.
    if (disk_) disk_->stop();
    for (auto& t : tracks_) {
        t->set_draining(false);
        t->writer().close();
    }
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
