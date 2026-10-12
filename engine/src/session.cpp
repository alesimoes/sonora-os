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

std::string take_file_name(const std::string& track, int take) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%02d", take);
    return track + "_" + digits + ".wav";
}

// The take number in `name` if it is exactly what start_recording() would
// write for one of `tracks`, or 0. Anything looser lets a stray file set the
// counter: "Guitar_20261012.wav" would make the next take 20261013. Only up
// to Session::kMaxTake digits are read, so no run of digits can overflow.
int take_of(const std::string& name,
            const std::vector<std::unique_ptr<Track>>& tracks) {
    const std::string ext = ".wav";
    if (name.size() <= ext.size() ||
        name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
        return 0;
    }
    const std::size_t end = name.size() - ext.size();
    const std::size_t under = name.rfind('_', end);
    if (under == std::string::npos) return 0;
    const std::size_t digits = end - under - 1;
    if (digits == 0 || digits > 4) return 0;  // 4 == digits in kMaxTake
    int n = 0;
    for (std::size_t k = under + 1; k < end; ++k) {
        if (name[k] < '0' || name[k] > '9') return 0;
        n = n * 10 + (name[k] - '0');
    }
    if (n < 1 || n > Session::kMaxTake) return 0;
    const std::string track = name.substr(0, under);
    for (const auto& t : tracks) {
        // Round-tripped rather than compared piecewise, so "Audio 1_1.wav"
        // and "Audio 1_0005.wav" -- not spellings we produce -- do not count.
        if (t->name() == track && take_file_name(track, n) == name) return n;
    }
    return 0;
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
        const int n = take_of(e->d_name, tracks_);
        if (n > highest) highest = n;
    }
    ::closedir(d);
    return highest + 1;
}

bool Session::start_recording(int take_number, std::string* error) {
    const uint64_t origin = transport_->playhead();
    std::vector<Track*> opened;

    if (take_number < 1 || take_number > kMaxTake) {
        const std::string why =
            "take " + std::to_string(take_number) + " is past the last take a"
            " session holds (" + std::to_string(kMaxTake) +
            "); start a new session";
        std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
        if (error) *error = why;
        return false;
    }

    for (auto& t : tracks_) {
        if (!t->rec_armed()) continue;
        const std::string file =
            audio_dir(cfg_.dir) + "/" + take_file_name(t->name(), take_number);
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
                file + ": " +
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
