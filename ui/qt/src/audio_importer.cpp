// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_importer.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace sonora::ui {
namespace fs = std::filesystem;
using namespace model;

namespace {

constexpr const char* kAudioDir = "Audio Files";

// A file's name without its folder and extension, fit to be a file name.
std::string stem_of(const std::string& path) {
    std::string s = fs::path(path).stem().string();
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("Imported") : s;
}

}  // namespace

AudioImporter::~AudioImporter() {
    cancel_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) worker_.join();
    discard_files();
}

std::size_t AudioImporter::most_files() { return kMaxCompoundMembers / 3; }

int AudioImporter::progress() const {
    const std::size_t n = items_.empty() ? 1 : items_.size();
    const std::size_t i = std::min(item_index_.load(std::memory_order_relaxed), n);
    if (state() == State::Done) return 1000;
    return static_cast<int>((i * 1000 + static_cast<std::size_t>(item_progress_.load(
                                            std::memory_order_relaxed))) / n);
}

std::string AudioImporter::current() const {
    const std::size_t i = item_index_.load(std::memory_order_relaxed);
    return i < items_.size() ? items_[i].stem : std::string();
}

bool AudioImporter::start(const std::vector<std::string>& files, const std::string& folder,
                          bool at_insertion, std::string& why) {
    if (state() == State::Running) {
        why = "an import is already running";
        return false;
    }
    reset();
    if (files.empty()) {
        why = "choose the files to import first";
        return false;
    }
    if (files.size() > most_files()) {
        why = "import at most " + std::to_string(most_files()) + " files at a time";
        return false;
    }
    if (folder.empty()) {
        why = "the engine has not said where this session's audio goes, so nothing was imported";
        return false;
    }
    const Snapshot& snap = ctl_.snapshot();
    rate_ = snap.settings.sample_rate;
    std::error_code ec;
    fs::create_directories(fs::path(folder) / kAudioDir, ec);
    if (ec) {
        why = "cannot use " + folder + "/" + kAudioDir + ": " + ec.message();
        return false;
    }

    std::vector<Item> items;
    for (const std::string& f : files) {
        AudioInfo info;
        std::string problem;
        if (!probe_audio(f, info, problem)) {
            why = fs::path(f).filename().string() + ": " + problem + ". Nothing was imported.";
            return false;
        }
        if (info.channels > kImportMaxChannels) {
            why = fs::path(f).filename().string() + " has " + std::to_string(info.channels) +
                  " channels; a track is mono or stereo. Split it into mono files first."
                  " Nothing was imported.";
            return false;
        }
        Item it;
        it.src = f;
        const std::string base = stem_of(f);
        // A name no source, file or earlier item of this import has.
        for (int n = 1;; ++n) {
            const std::string name = n == 1 ? base : base + " " + std::to_string(n);
            const std::string rel = std::string(kAudioDir) + "/" + name + ".wav";
            const fs::path abs = fs::path(folder) / rel;
            const auto same = [&](const std::string& r) { return r == rel; };
            const bool pooled = std::any_of(snap.sources->sources.begin(), snap.sources->sources.end(),
                                            [&](const Source& s) { return same(s.path); });
            const bool batch = std::any_of(items.begin(), items.end(),
                                           [&](const Item& o) { return same(o.rel); });
            if (!pooled && !batch && !fs::exists(abs, ec) &&
                !fs::exists(fs::path(abs).concat(".part"), ec)) {
                it.stem = name;
                it.rel = rel;
                it.final_path = abs.string();
                it.part = it.final_path + ".part";
                break;
            }
        }
        items.push_back(std::move(it));
    }

    items_ = std::move(items);
    folder_ = folder;
    at_insertion_ = at_insertion;
    insertion_ = ctl_.selection().insertion;
    failure_.clear();
    cancel_.store(false);
    item_progress_.store(0);
    item_index_.store(0);
    state_.store(State::Running, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
    return true;
}

void AudioImporter::run() {
    State end = State::Done;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        item_index_.store(i, std::memory_order_relaxed);
        item_progress_.store(0, std::memory_order_relaxed);
        Item& it = items_[i];
        ImportResult r;
        std::string why;
        if (!import_audio(it.src, it.part, rate_, &item_progress_, &cancel_, r, why)) {
            if (why.empty()) {
                end = State::Cancelled;
            } else {
                failure_ = fs::path(it.src).filename().string() + ": " + why +
                           ". Nothing was imported.";
                end = State::Failed;
            }
            break;
        }
        it.written = true;
        it.channels = r.channels;
        it.frames = r.frames;
        it.floating = r.floating;
        it.time_reference = r.time_reference;
    }
    if (end != State::Done) {
        for (Item& it : items_) {
            if (it.written) {
                std::error_code ec;
                fs::remove(it.part, ec);
                it.written = false;
            }
        }
    }
    state_.store(end, std::memory_order_release);
}

void AudioImporter::discard_files() {
    for (Item& it : items_) {
        std::error_code ec;
        fs::remove(it.part, ec);
        fs::remove(it.final_path, ec);
    }
    items_.clear();
}

void AudioImporter::reset() {
    if (worker_.joinable()) worker_.join();
    // A finished run nobody committed: its copies are not in the session.
    if (state() == State::Done) discard_files();
    items_.clear();
    state_.store(State::Idle, std::memory_order_release);
}

bool AudioImporter::commit(const std::string& folder, std::string& why) {
    if (state() != State::Done) {
        why = "no import has finished";
        return false;
    }
    if (worker_.joinable()) worker_.join();
    const auto refuse = [&](const std::string& reason) {
        discard_files();
        state_.store(State::Idle, std::memory_order_release);
        why = reason;
        return false;
    };
    const Snapshot& s0 = ctl_.snapshot();
    if (folder != folder_ || s0.settings.sample_rate != rate_) {
        return refuse("the session changed while the files were converting; nothing was imported");
    }
    for (const Item& it : items_) {
        std::error_code ec;
        fs::rename(it.part, it.final_path, ec);
        if (ec) {
            return refuse("cannot place " + it.rel + ": " + ec.message() + "; nothing was imported");
        }
    }

    const Snapshot& s = ctl_.snapshot();
    std::vector<std::string> names;
    std::vector<std::uint16_t> inputs;
    for (const auto& t : s.edit->tracks) {
        names.push_back(t->name);
        const TrackMix* m = s.mix.find(t->id);
        if (t->kind == TrackKind::Audio && m != nullptr) {
            for (std::uint16_t in : m->input) inputs.push_back(in);
        }
    }
    const auto has = [](const auto& list, const auto& v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    TimelineDocument& doc = ctl_.document();
    std::vector<Member> members;
    const Frames at = at_insertion_ ? insertion_ : 0;
    std::uint16_t next_input = 0;
    for (const Item& it : items_) {
        std::string name = it.stem;
        for (int n = 2; has(names, name); ++n) name = it.stem + " " + std::to_string(n);
        names.push_back(name);

        AddTrack t;
        t.track = doc.reserve_id();
        t.playlist = doc.reserve_id();
        t.name = name;
        t.playlist_name = name + ".01";
        t.channels = it.channels;
        for (std::uint16_t c = 0; c < it.channels; ++c) {
            while (has(inputs, next_input)) ++next_input;
            inputs.push_back(next_input);
            t.mix.input.push_back(next_input);
        }

        AddSource src;
        src.source.id = doc.reserve_id();
        src.source.path = it.rel;
        src.source.channels = it.channels;
        src.source.length = it.frames;
        src.source.format = it.floating ? AudioFormat::Float32 : AudioFormat::Int24;
        src.source.time_reference = it.time_reference;
        src.source.kind = SourceKind::Imported;

        PlaceRegion p;
        p.playlist = t.playlist;
        p.region.id = doc.reserve_id();
        p.region.name = it.stem;
        p.region.position = at;
        p.region.length = it.frames;
        for (std::uint16_t c = 0; c < it.channels; ++c) p.region.channels.push_back({src.source.id, c});

        members.push_back(std::move(t));
        members.push_back(std::move(src));
        members.push_back(std::move(p));
    }
    const std::size_t count = items_.size();
    const int first_new = static_cast<int>(s.edit->tracks.size());
    std::string problem;
    if (!doc.submit(Compound{std::move(members), count == 1 ? "Import Audio" : "Import Audio Files"},
                     problem)) {
        return refuse("the session refused the import: " + problem +
                      ". The copies were deleted.");
    }
    items_.clear();
    state_.store(State::Idle, std::memory_order_release);
    ctl_.select_track(first_new);
    return true;
}

}  // namespace sonora::ui
