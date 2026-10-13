// SPDX-License-Identifier: GPL-3.0-or-later
#include "measure.h"

#include <QMap>
#include <QQuickWindow>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <ctime>

#include "sonora/platform/thread_cpu.h"

namespace sonora::ui {

namespace {

int64_t mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// std::clock() is process CPU time on glibc (CLOCK_PROCESS_CPUTIME_ID), so the
// column is unchanged on the appliance. MSVC's std::clock() is wall time; a
// Windows build would need process CPU behind common/platform too.
int64_t process_cpu_ns() {
    return int64_t(double(std::clock()) * (1e9 / double(CLOCKS_PER_SEC)));
}

// CPU time per thread name. Threads of the same name (llvmpipe's rasteriser
// pool) are summed, so the report says what kind of work cost what.
struct ThreadsCpu {
    bool available = false;
    QString unavailable_reason;
    int64_t resolution_ns = 0;
    QMap<QString, int64_t> by_name;
};

ThreadsCpu threads_cpu_ns() {
    const platform::ThreadCpuTimes t = platform::thread_cpu_times();
    ThreadsCpu out;
    out.available = t.available;
    out.unavailable_reason = QString::fromStdString(t.unavailable_reason);
    out.resolution_ns = t.resolution_ns;
    for (const platform::ThreadCpu& th : t.threads) {
        QString name = QString::fromStdString(th.name);
        // "llvmpipe-3" and "llvmpipe-11" are one pool.
        while (!name.isEmpty() && (name.back().isDigit() || name.back() == QLatin1Char('-'))) {
            name.chop(1);
        }
        out.by_name[name] += th.cpu_ns;
    }
    return out;
}

ThreadsCpu g_threads_at_begin;

struct Pcts {
    double p50 = 0, p99 = 0, max = 0;
    std::size_t n = 0;
};

Pcts percentiles(std::vector<int64_t> v) {
    Pcts p;
    p.n = v.size();
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    const auto at = [&v](double q) {
        const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * double(v.size())));
        return double(v[i]) / 1e6;
    };
    p.p50 = at(0.50);
    p.p99 = at(0.99);
    p.max = double(v.back()) / 1e6;
    return p;
}

QString line(const char* what, const Pcts& p) {
    return QString::asprintf("  %-34s n=%-6zu p50 %7.3f ms   p99 %7.3f ms   max %7.3f ms\n",
                             what, p.n, p.p50, p.p99, p.max);
}

}  // namespace

FrameMeter::FrameMeter(std::size_t capacity) { samples_.resize(capacity); }

void FrameMeter::attach(QQuickWindow* window) {
    QObject::connect(
        window, &QQuickWindow::beforeRendering, window,
        [this]() { render_start_ns_ = mono_ns(); }, Qt::DirectConnection);
    QObject::connect(
        window, &QQuickWindow::afterRendering, window,
        [this]() {
            if (!running_.load(std::memory_order_acquire)) return;
            const std::size_t i = count_.load(std::memory_order_relaxed);
            if (i >= samples_.size()) return;
            samples_[i].render_ns = mono_ns() - render_start_ns_;
            samples_[i].swapped_ns = 0;
        },
        Qt::DirectConnection);
    QObject::connect(
        window, &QQuickWindow::frameSwapped, window,
        [this]() {
            if (!running_.load(std::memory_order_acquire)) return;
            const std::size_t i = count_.load(std::memory_order_relaxed);
            if (i >= samples_.size()) return;
            samples_[i].swapped_ns = mono_ns();
            count_.store(i + 1, std::memory_order_release);
        },
        Qt::DirectConnection);
}

void FrameMeter::begin() {
    g_threads_at_begin = threads_cpu_ns();
    begin_wall_ns_ = mono_ns();
    begin_cpu_ns_ = process_cpu_ns();
    count_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
}

QString FrameMeter::report(const TickStats& ticks, const QString& label, int meters,
                           int meter_hz, const QString& render_path) {
    running_.store(false, std::memory_order_release);
    const int64_t wall = mono_ns() - begin_wall_ns_;
    const int64_t cpu = process_cpu_ns() - begin_cpu_ns_;
    const ThreadsCpu threads_end = threads_cpu_ns();
    const std::size_t n = count_.load(std::memory_order_acquire);

    std::vector<int64_t> render, interval;
    for (std::size_t i = 0; i < n; ++i) {
        render.push_back(samples_[i].render_ns);
        if (i > 0) interval.push_back(samples_[i].swapped_ns - samples_[i - 1].swapped_ns);
    }

    QString out;
    QTextStream s(&out);
    s << "sonora-ui --measure: " << label << "\n";
    s << "  rendering path: " << render_path << "\n";
    s << "  meters attached: " << meters << ", meter tick " << meter_hz << " Hz\n";
    s << QString::asprintf("  interval: %.2f s, %zu frames swapped = %.1f fps\n",
                           double(wall) / 1e9, n, double(n) / (double(wall) / 1e9));
    s << line("frame-to-frame (frameSwapped)", percentiles(interval));
    s << line("render (before->afterRendering)", percentiles(render));
    s << line("GUI tick (pull + feed 18 meters)", percentiles(ticks.tick_ns));
    s << line("GUI tick interval", percentiles(ticks.interval_ns));
    s << QString::asprintf("  process CPU: %.1f %% of one core (%.3f s CPU over %.3f s)\n",
                           100.0 * double(cpu) / double(wall), double(cpu) / 1e9,
                           double(wall) / 1e9);
    if (!threads_end.available || !g_threads_at_begin.available) {
        s << "  per thread: unavailable (" << threads_end.unavailable_reason << ")\n";
        return out;
    }
    s << QString::asprintf("  per thread (CPU %% of one core, %g ms resolution):\n",
                           double(threads_end.resolution_ns) / 1e6);
    const QMap<QString, int64_t>& end_by_name = threads_end.by_name;
    for (auto it = end_by_name.begin(); it != end_by_name.end(); ++it) {
        const int64_t d = it.value() - g_threads_at_begin.by_name.value(it.key(), 0);
        if (d <= 0) continue;
        s << QString::asprintf("    %-20s %6.1f %%\n", qPrintable(it.key()),
                               100.0 * double(d) / double(wall));
    }
    return out;
}

}  // namespace sonora::ui
