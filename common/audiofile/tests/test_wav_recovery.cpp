// SPDX-License-Identifier: GPL-3.0-or-later
//
// A take whose engine died before closing it (session-format.md, "Crash
// recovery"): its header says less than the file holds, zero if no
// checkpoint ever ran. recover_take() gives it the length on disk, in whole
// frames; it never touches a file that is finished, short, or not ours.
#include "sonora/wav_recovery.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::TakeRecovery;
using sonora::WavWriter;

// Where the writer puts them: RIFF ckSize at 4, the data ckSize just
// before the first sample (bext 602 and fmt 16 bytes before it).
constexpr long kRiffSize = 4;
constexpr long kDataSize = 12 + (8 + 602) + (8 + 16) + 4;
constexpr long kAudio = kDataSize + 4;

std::vector<uint8_t> slurp(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> out(n > 0 ? static_cast<std::size_t>(n) : 0);
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    std::fclose(f);
    return out;
}

void spill(const std::string& path, const std::vector<uint8_t>& b) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

void put32(std::vector<uint8_t>& b, long off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }

// A finished take of `frames` frames, sample i = i / 1024 (exact in 24-bit
// and in float), at TimeReference 4800.
std::vector<uint8_t> finished(const std::string& path, uint16_t channels, WavWriter::Format f,
                              std::size_t frames) {
    std::remove(path.c_str());
    WavWriter w;
    w.open(path, 48000, channels, f, 4800);
    std::vector<float> s(frames * channels);
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(i % 1000) / 1024.0f;
    w.write(s.data(), frames);
    w.close();
    return slurp(path);
}

// The file as the engine left it if it died before any checkpoint: the
// audio on disk, both sizes still the zeros open() wrote, no pad byte.
std::vector<uint8_t> unfinalised(std::vector<uint8_t> b, std::size_t data_bytes) {
    b.resize(static_cast<std::size_t>(kAudio) + data_bytes);
    put32(b, kRiffSize, 0);
    put32(b, kDataSize, 0);
    return b;
}

bool samples_match(const std::string& path, int64_t frames, uint16_t channels) {
    sonora::WavReader r;
    std::string error;
    if (!r.open(path, error) || r.frames() != frames || r.channels() != channels ||
        r.time_reference() != 4800) {
        return false;
    }
    std::vector<float> s(static_cast<std::size_t>(frames) * channels);
    if (r.read(0, frames, s.data()) != frames) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != static_cast<float>(i % 1000) / 1024.0f) return false;
    }
    return true;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_wav_recovery";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "Audio 1_01.wav").string();

    // --- no checkpoint ever ran: sizes zero, the audio all there ---
    {
        const auto good = finished(path, 2, WavWriter::Format::Float32, 300);
        spill(path, unfinalised(good, 300 * 8));
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Repaired);
        CHECK(r.header_frames == 0);
        CHECK(r.frames == 300);
        // Exactly what close() would have written.
        CHECK(slurp(path) == good);
        CHECK(samples_match(path, 300, 2));
    }

    // --- a checkpoint ran, then more audio reached the file ---
    {
        const auto good = finished(path, 1, WavWriter::Format::Int24, 1000);
        auto b = good;
        put32(b, kDataSize, 600 * 3);
        put32(b, kRiffSize, static_cast<uint32_t>(kAudio - 8 + 600 * 3));
        spill(path, b);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Repaired);
        CHECK(r.header_frames == 600);
        CHECK(r.frames == 1000);
        CHECK(slurp(path) == good);
    }

    // --- part of a frame at the end is dropped; an odd chunk gets its pad ---
    //
    // Mono 24-bit, 5 frames: 15 bytes of data, then two bytes of a sixth
    // frame the engine died writing.
    {
        const auto good = finished(path, 1, WavWriter::Format::Int24, 5);
        CHECK(good.size() == static_cast<std::size_t>(kAudio) + 15 + 1);
        auto b = unfinalised(good, 15);
        b.push_back(0x7f);
        b.push_back(0x7f);
        spill(path, b);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Repaired);
        CHECK(r.frames == 5);
        CHECK(slurp(path) == good);
        CHECK(samples_match(path, 5, 1));

        // A checkpoint's header on odd data has no pad yet; that alone is
        // finished here.
        b = good;
        b.pop_back();
        put32(b, kRiffSize, static_cast<uint32_t>(b.size() - 8));
        spill(path, b);
        CHECK(sonora::recover_take(path).outcome == TakeRecovery::Outcome::Repaired);
        CHECK(slurp(path) == good);
    }

    // --- a finished take is left byte for byte as it is ---
    {
        const auto good = finished(path, 2, WavWriter::Format::Int24, 77);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Intact);
        CHECK(r.frames == 77);
        CHECK(slurp(path) == good);
    }

    // --- a finished take another tool appended a chunk to is left alone ---
    //
    // BWF MetaEdit and taggers keep bext and add LIST or iXML after the
    // data, updating the RIFF size. Read as audio, the bytes of "LIST" are a
    // float sample of about 3.6e12: a full-scale burst on the monitor bus.
    {
        auto b = finished(path, 1, WavWriter::Format::Float32, 100);
        const uint8_t list[30] = {'L', 'I', 'S', 'T', 22, 0, 0, 0, 'I', 'N', 'F', 'O',
                                  'I', 'C', 'M', 'T', 10, 0, 0, 0, 'o', 'v', 'e', 'r',
                                  'd', 'u', 'b', ' ', '2', 0};
        b.insert(b.end(), list, list + sizeof list);
        put32(b, kRiffSize, static_cast<uint32_t>(b.size() - 8));
        spill(path, b);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Intact);
        CHECK(r.frames == 100);
        CHECK(slurp(path) == b);
        CHECK(samples_match(path, 100, 1));
    }

    // --- a take that failed and was closed is finished, not recovered ---
    //
    // Mono 24-bit, odd frame count, stopped by a failure: close() writes the
    // RIFF size with the pad byte counted. Whether or not the pad reached
    // the file, recovery must not report it as a take the engine died
    // recording.
    {
        std::remove(path.c_str());
        WavWriter w;
        w.set_riff_size_limit(kAudio - 8 + 16);
        CHECK(w.open(path, 48000, 1, WavWriter::Format::Int24, 4800));
        std::vector<float> s(6);
        for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(i) / 1024.0f;
        CHECK(!w.write(s.data(), 6));
        CHECK(w.failed());
        w.close();
        const auto failed = slurp(path);
        CHECK(failed.size() == static_cast<std::size_t>(kAudio) + 15 + 1);
        TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Intact);
        CHECK(r.frames == 5);
        CHECK(slurp(path) == failed);
        CHECK(samples_match(path, 5, 1));

        // The pad's resize failed too: the RIFF size is still close()'s.
        auto b = failed;
        b.pop_back();
        spill(path, b);
        r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Intact);
        CHECK(slurp(path) == b);
    }

    // --- a RIFF size neither open() nor a checkpoint writes: untouched ---
    {
        auto b = finished(path, 1, WavWriter::Format::Float32, 10);
        b.insert(b.end(), 12, 0x41);
        put32(b, kRiffSize, static_cast<uint32_t>(b.size() - 8 - 4));
        spill(path, b);
        CHECK(sonora::recover_take(path).outcome == TakeRecovery::Outcome::Intact);
        CHECK(slurp(path) == b);
    }

    // --- not ours: another program's file, whatever its sizes say ---
    //
    // A WAV may legally carry chunks after its data; only the engine's own
    // takes are known to end with it.
    {
        auto b = unfinalised(finished(path, 1, WavWriter::Format::Float32, 10), 40);
        std::memcpy(b.data() + 12 + 8 + 256, "Other DAW    ", 13);
        spill(path, b);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::NotOurs);
        CHECK(slurp(path) == b);
    }

    // --- shorter than its header: reported, never "repaired" shorter ---
    {
        auto b = finished(path, 1, WavWriter::Format::Float32, 10);
        b.resize(b.size() - 8);
        spill(path, b);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Short);
        CHECK(!r.why.empty());
        CHECK(slurp(path) == b);
    }

    // --- not a WAV, or a header cut short: unreadable, untouched ---
    {
        std::vector<uint8_t> junk(100, 0x41);
        spill(path, junk);
        CHECK(sonora::recover_take(path).outcome == TakeRecovery::Outcome::Unreadable);
        CHECK(slurp(path) == junk);
        auto b = finished(path, 1, WavWriter::Format::Float32, 10);
        b.resize(200);
        spill(path, b);
        const TakeRecovery r = sonora::recover_take(path);
        CHECK(r.outcome == TakeRecovery::Outcome::Unreadable);
        CHECK(!r.why.empty());
        CHECK(slurp(path) == b);
    }

    // --- a folder: every .wav looked at, only the unfinished ones listed ---
    {
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string a = (dir / "Audio 1_01.wav").string();
        const std::string c = (dir / "Audio 2_01.wav").string();
        finished(a, 1, WavWriter::Format::Int24, 50);
        spill(c, unfinalised(finished(c, 1, WavWriter::Format::Int24, 50), 150));
        spill((dir / "notes.txt").string(), {'h', 'i'});
        const std::vector<TakeRecovery> found = sonora::recover_takes(dir.string());
        CHECK(found.size() == 1);
        CHECK(found[0].path == c);
        CHECK(found[0].outcome == TakeRecovery::Outcome::Repaired);
        CHECK(found[0].frames == 50);
        CHECK(sonora::recover_takes(dir.string()).empty());
        // A folder that is not there yet: a new session.
        CHECK(sonora::recover_takes((dir / "missing").string()).empty());
    }

    fs::remove_all(dir);
    std::printf("test_wav_recovery: ok\n");
    return 0;
}
