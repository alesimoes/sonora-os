// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine.record.direct_io (BACKLOG, "measure before changing"): the disk
// thread as it is, WavWriter's unbuffered stdio through the page cache with
// a sync every checkpoint, against O_DIRECT with aligned buffers; and its
// 10 ms poll against a semaphore the callback posts. Measured by hand (the
// figures belong to the machine and the medium), built with the tests so it
// cannot rot, never run by ctest. Results: brain/research/disk-write-path.md.
//
//   measure_disk_write DIR [tracks=18] [seconds=20] [throughput_mib=256]
//
// DIR must be on the medium being measured and support O_DIRECT (not tmpfs,
// not a 9p or drvfs share).

#include <fcntl.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/wav_writer.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kRate = 48000;
// The disk thread's poll, and so the audio one pass carries.
constexpr std::size_t kPassFrames = kRate / 100;
// A 128-frame callback at 48 kHz, what the UMC1820 runs at (ADR-0009).
constexpr std::size_t kCallbackFrames = 128;
constexpr std::size_t kAlign = 4096;
constexpr auto kCheckpoint = std::chrono::seconds(2);

int64_t ns_since(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

double cpu_seconds() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
           static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

struct Stats {
    std::vector<int64_t> ns;
    void add(int64_t v) { ns.push_back(v); }
    int64_t pct(double p) {
        if (ns.empty()) return 0;
        std::sort(ns.begin(), ns.end());
        const auto i = static_cast<std::size_t>(p * static_cast<double>(ns.size() - 1));
        return ns[i];
    }
};

// The writer's own 24-bit packing, so both paths cost the same per sample.
void pack24(const float* in, std::size_t n, uint8_t* out) {
    for (std::size_t i = 0; i < n; ++i) {
        const float s = std::max(-1.0f, std::min(1.0f, in[i])) * 8388607.0f;
        const auto v = static_cast<int32_t>(std::lrintf(s));
        out[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
        out[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    }
}

// One mono track's file as the disk thread would hold it.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const float* in, std::size_t frames) = 0;
    virtual bool sync() = 0;
    virtual bool close() = 0;
    // Audio the process holds that the kernel does not: lost to kill -9.
    virtual std::size_t held_bytes() const { return 0; }
};

class StdioSink final : public Sink {
public:
    explicit StdioSink(const std::string& path) {
        ok_ = w_.open(path, kRate, 1, sonora::WavWriter::Format::Int24, 0);
    }
    bool ok() const { return ok_; }
    bool write(const float* in, std::size_t frames) override { return w_.write(in, frames); }
    bool sync() override { return w_.checkpoint(); }
    bool close() override { return w_.close(); }

private:
    sonora::WavWriter w_;
    bool ok_ = false;
};

// O_DIRECT: every write a multiple of the block size from an aligned buffer,
// so the tail short of a block waits in user memory until the next pass or
// the close. No WAV header: only the audio path is compared.
class DirectSink final : public Sink {
public:
    DirectSink(const std::string& path, std::size_t capacity) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_DIRECT | O_CLOEXEC, 0644);
        void* p = nullptr;
        if (posix_memalign(&p, kAlign, capacity) == 0) buf_.reset(static_cast<uint8_t*>(p));
        cap_ = capacity;
    }
    ~DirectSink() override { close(); }
    bool ok() const { return fd_ >= 0 && buf_; }
    bool write(const float* in, std::size_t frames) override {
        if (fill_ + frames * 3 > cap_) return false;
        pack24(in, frames, buf_.get() + fill_);
        fill_ += frames * 3;
        const std::size_t whole = fill_ / kAlign * kAlign;
        if (whole == 0) return true;
        if (::write(fd_, buf_.get(), whole) != static_cast<ssize_t>(whole)) return false;
        std::memmove(buf_.get(), buf_.get() + whole, fill_ - whole);
        fill_ -= whole;
        return true;
    }
    // O_DIRECT bypasses the page cache, but the inode's size and the
    // device's write cache still need the flush.
    bool sync() override { return ::fdatasync(fd_) == 0; }
    bool close() override {
        if (fd_ < 0) return true;
        // The last part block: written through the cache, as a real close
        // would have to.
        bool ok = true;
        if (fill_ > 0) {
            const int flags = fcntl(fd_, F_GETFL);
            ok = fcntl(fd_, F_SETFL, flags & ~O_DIRECT) == 0 &&
                 ::write(fd_, buf_.get(), fill_) == static_cast<ssize_t>(fill_);
        }
        ok = ::close(fd_) == 0 && ok;
        fd_ = -1;
        return ok;
    }
    std::size_t held_bytes() const override { return fill_; }

private:
    struct Free {
        void operator()(uint8_t* p) const { std::free(p); }
    };
    int fd_ = -1;
    std::unique_ptr<uint8_t, Free> buf_;
    std::size_t cap_ = 0;
    std::size_t fill_ = 0;
};

std::vector<std::unique_ptr<Sink>> open_sinks(const std::string& dir, const char* tag,
                                              int tracks, bool direct) {
    std::vector<std::unique_ptr<Sink>> out;
    for (int t = 0; t < tracks; ++t) {
        const std::string path = dir + "/measure-" + tag + "-" + std::to_string(getpid()) + "-" +
                                 std::to_string(t) + ".wav";
        std::remove(path.c_str());
        if (direct) {
            auto s = std::make_unique<DirectSink>(path, 4 * 1024 * 1024);
            if (!s->ok()) {
                std::fprintf(stderr, "open %s O_DIRECT: %s\n", path.c_str(), std::strerror(errno));
                return {};
            }
            out.push_back(std::move(s));
        } else {
            auto s = std::make_unique<StdioSink>(path);
            if (!s->ok()) {
                std::fprintf(stderr, "open %s: %s\n", path.c_str(), std::strerror(errno));
                return {};
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

void remove_files(const std::string& dir, const char* tag, int tracks) {
    for (int t = 0; t < tracks; ++t) {
        const std::string path = dir + "/measure-" + tag + "-" + std::to_string(getpid()) + "-" +
                                 std::to_string(t) + ".wav";
        std::remove(path.c_str());
    }
}

// Real-time paced: one pass of every track each 10 ms, a checkpoint every
// 2 s, as DiskThread::run() does.
int paced(const std::string& dir, int tracks, int seconds, bool direct) {
    const char* tag = direct ? "direct" : "stdio";
    auto sinks = open_sinks(dir, tag, tracks, direct);
    if (sinks.empty()) return 1;
    std::vector<float> audio(kPassFrames);
    for (std::size_t i = 0; i < audio.size(); ++i) {
        audio[i] = 0.5f * std::sin(static_cast<float>(i) * 0.0572f);
    }
    Stats pass, sync;
    std::size_t held_max = 0;
    const double cpu0 = cpu_seconds();
    const auto start = Clock::now();
    auto next = start;
    auto next_sync = start + kCheckpoint;
    const int passes = seconds * 100;
    bool ok = true;
    for (int p = 0; p < passes && ok; ++p) {
        const auto t0 = Clock::now();
        for (auto& s : sinks) ok = s->write(audio.data(), audio.size()) && ok;
        pass.add(ns_since(t0));
        std::size_t held = 0;
        for (auto& s : sinks) held += s->held_bytes();
        held_max = std::max(held_max, held);
        if (Clock::now() >= next_sync) {
            const auto t1 = Clock::now();
            for (auto& s : sinks) ok = s->sync() && ok;
            sync.add(ns_since(t1));
            next_sync = Clock::now() + kCheckpoint;
        }
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
    for (auto& s : sinks) ok = s->close() && ok;
    const double cpu = cpu_seconds() - cpu0;
    remove_files(dir, tag, tracks);
    std::printf("paced %-6s tracks=%d s=%d ok=%d pass_us p50=%.1f p99=%.1f max=%.1f "
                "sync_ms max=%.2f n=%zu cpu_pct=%.2f held_max_bytes=%zu\n",
                tag, tracks, seconds, ok ? 1 : 0, pass.pct(0.5) / 1e3, pass.pct(0.99) / 1e3,
                pass.pct(1.0) / 1e3, sync.pct(1.0) / 1e6, sync.ns.size(),
                100.0 * cpu / static_cast<double>(seconds), held_max);
    return ok ? 0 : 1;
}

// As fast as the medium takes it: the headroom over real time.
int throughput(const std::string& dir, int tracks, int mib, bool direct) {
    const char* tag = direct ? "direct" : "stdio";
    auto sinks = open_sinks(dir, tag, tracks, direct);
    if (sinks.empty()) return 1;
    // 64 KiB of samples per write, DiskThread's chunk.
    std::vector<float> audio(65536, 0.25f);
    const uint64_t total = static_cast<uint64_t>(mib) * 1024 * 1024;
    const uint64_t per_write = audio.size() * 3;
    uint64_t written = 0;
    bool ok = true;
    const double cpu0 = cpu_seconds();
    const auto t0 = Clock::now();
    auto next_sync = t0 + kCheckpoint;
    while (written < total && ok) {
        for (auto& s : sinks) {
            ok = s->write(audio.data(), audio.size()) && ok;
            written += per_write;
        }
        if (Clock::now() >= next_sync) {
            for (auto& s : sinks) ok = s->sync() && ok;
            next_sync = Clock::now() + kCheckpoint;
        }
    }
    for (auto& s : sinks) ok = s->sync() && s->close() && ok;
    const double secs = static_cast<double>(ns_since(t0)) / 1e9;
    const double cpu = cpu_seconds() - cpu0;
    remove_files(dir, tag, tracks);
    const double mbps = static_cast<double>(written) / (1024.0 * 1024.0) / secs;
    // One mono 24-bit 48 kHz track is 0.1373 MiB/s.
    std::printf("throughput %-6s tracks=%d ok=%d mib=%.0f MiB_s=%.1f realtime_tracks=%.0f "
                "cpu_s=%.2f\n",
                tag, tracks, ok ? 1 : 0, static_cast<double>(written) / (1024.0 * 1024.0), mbps,
                mbps / (kRate * 3.0 / (1024.0 * 1024.0)), cpu);
    return ok ? 0 : 1;
}

// The callback's side of the wake: a fill count it advances every 128
// frames, and, signalled, a sem_post. The consumer polls every 10 ms or
// waits on the semaphore. What matters: the ring's high-water mark, how
// often the disk thread wakes, and what the post costs the callback.
int wake(int tracks, int seconds, bool signalled) {
    sem_t sem;
    sem_init(&sem, 0, 0);
    std::atomic<uint64_t> produced{0};
    std::atomic<bool> done{false};
    uint64_t high_water = 0;
    uint64_t wakeups = 0;
    std::thread consumer([&] {
        uint64_t consumed = 0;
        while (!done.load(std::memory_order_acquire)) {
            if (signalled) {
                timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100'000'000;
                if (ts.tv_nsec >= 1'000'000'000) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1'000'000'000;
                }
                sem_timedwait(&sem, &ts);
                // Every post since: one drain takes them all.
                while (sem_trywait(&sem) == 0) {
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ++wakeups;
            const uint64_t now = produced.load(std::memory_order_acquire);
            high_water = std::max(high_water, now - consumed);
            consumed = now;
        }
    });
    Stats post;
    const auto period = std::chrono::nanoseconds(1'000'000'000LL * kCallbackFrames / kRate);
    const auto start = Clock::now();
    auto next = start;
    const auto end = start + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        produced.fetch_add(kCallbackFrames, std::memory_order_release);
        if (signalled) {
            const auto t0 = Clock::now();
            sem_post(&sem);
            post.add(ns_since(t0));
        }
        next += period;
        std::this_thread::sleep_until(next);
    }
    done.store(true, std::memory_order_release);
    sem_post(&sem);
    consumer.join();
    sem_destroy(&sem);
    // Frames per track; the bytes are what every track's float ring holds.
    std::printf("wake %-9s s=%d high_water_frames=%llu (%.1f ms, %.0f KiB over %d tracks) "
                "wakeups_s=%.0f post_ns p50=%lld p99=%lld max=%lld\n",
                signalled ? "signalled" : "poll10ms", seconds,
                static_cast<unsigned long long>(high_water),
                1e3 * static_cast<double>(high_water) / kRate,
                static_cast<double>(high_water) * 4.0 * tracks / 1024.0, tracks,
                static_cast<double>(wakeups) / seconds, static_cast<long long>(post.pct(0.5)),
                static_cast<long long>(post.pct(0.99)), static_cast<long long>(post.pct(1.0)));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s DIR [tracks=18] [seconds=20] [throughput_mib=256]\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const int tracks = argc > 2 ? std::atoi(argv[2]) : 18;
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 20;
    const int mib = argc > 4 ? std::atoi(argv[4]) : 256;
    if (tracks < 1 || seconds < 1 || mib < 1) return 2;
    int rc = 0;
    rc |= paced(dir, tracks, seconds, false);
    rc |= paced(dir, tracks, seconds, true);
    rc |= throughput(dir, tracks, mib, false);
    rc |= throughput(dir, tracks, mib, true);
    rc |= wake(tracks, seconds, false);
    rc |= wake(tracks, seconds, true);
    std::printf("RESULT %s\n", rc == 0 ? "ok" : "failed");
    return rc;
}
