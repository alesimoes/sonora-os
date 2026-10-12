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

std::string track_name(int number) {
    char name[32];
    std::snprintf(name, sizeof name, "Audio %d", number);
    return name;
}

// The number in a run of decimal digits, or 0 if the run is empty, holds
// anything else, or is longer than `max_digits`. The bound is what keeps a
// run of any length from overflowing.
int parse_number(const std::string& s, std::size_t begin, std::size_t end,
                 std::size_t max_digits) {
    if (end <= begin || end - begin > max_digits) return 0;
    int n = 0;
    for (std::size_t k = begin; k < end; ++k) {
        if (s[k] < '0' || s[k] > '9') return 0;
        n = n * 10 + (s[k] - '0');
    }
    return n;
}

// The take number in `name` if it is exactly what start_recording() would
// write for some track of a session, or 0. Any "Audio <n>" counts, not only
// the tracks open now: the same folder may have been recorded with more.
// Anything looser lets a stray file set the counter: "Guitar_20261012.wav"
// would make the next take 20261013.
int take_of(const std::string& name) {
    const std::string ext = ".wav";
    if (name.size() <= ext.size() ||
        name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
        return 0;
    }
    const std::size_t end = name.size() - ext.size();
    const std::size_t under = name.rfind('_', end);
    if (under == std::string::npos) return 0;
    const int take = parse_number(name, under + 1, end, 4);  // 4 == digits in kMaxTake
    if (take < 1 || take > Session::kMaxTake) return 0;

    const std::string prefix = "Audio ";
    if (name.compare(0, prefix.size(), prefix) != 0) return 0;
    const int track = parse_number(name, prefix.size(), under, 9);
    if (track < 1) return 0;
    // Round-tripped rather than compared piecewise, so "Audio 1_1.wav",
    // "Audio 01_05.wav" and "Audio 1_0005.wav" -- not spellings we produce --
    // do not count.
    return take_file_name(track_name(track), take) == name ? take : 0;
}

}  // namespace

Session::Session(SessionConfig cfg) : cfg_(std::move(cfg)) {
    transport_ = std::make_unique<Transport>(cfg_.sample_rate);

    const auto ring_frames =
        static_cast<std::size_t>(cfg_.ring_seconds * cfg_.sample_rate);

    std::vector<Track*> raw;
    for (int i = 0; i < cfg_.track_count; ++i) {
        tracks_.push_back(std::make_unique<Track>(i, track_name(i + 1),
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
    // track, armed now, open now, or neither.
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
        // The disk thread reads this ring only once the track is published
        // below, a release store that comes after this reset. The audio
        // thread is not fenced off the same way. process() loads the
        // transport state once per block and writes the ring later, so a
        // callback that loaded Recording for the previous take and was then
        // descheduled can still write while this runs: a data race on the
        // indices, and old samples at the head of the new take. Reaching
        // that needs the RT thread stalled across all of stop() (setting
        // Stopped, joining the disk thread, closing every file) and the
        // operator's next record command, ~100 ms at the least, dozens of
        // missed periods. It is not ruled out, only made implausible; a
        // handshake with the callback (it acknowledges a take epoch) is
        // what would rule it out.
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
