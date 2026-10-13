// SPDX-License-Identifier: GPL-3.0-or-later
#include "measure.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QQuickWindow>
#include <QTextStream>
#include <algorithm>
#include <ctime>
#include <unistd.h>

namespace sonora::ui {

namespace {

int64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

int64_t process_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// CPU time per thread name, from /proc. Threads of the same name (llvmpipe's
// rasteriser pool) are summed, so the report says what kind of work cost what.
QMap<QString, int64_t> threads_cpu_ns() {
    QMap<QString, int64_t> out;
    const long tick = sysconf(_SC_CLK_TCK);
    const QDir dir(QStringLiteral("/proc/self/task"));
    for (const QString& tid : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile f(dir.filePath(tid + QStringLiteral("/stat")));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString line = QString::fromLatin1(f.readAll());
        const int open = line.indexOf(QLatin1Char('('));
        const int close = line.lastIndexOf(QLatin1Char(')'));
        if (open < 0 || close < 0) continue;
        QString name = line.mid(open + 1, close - open - 1);
        // "llvmpipe-3" and "llvmpipe-11" are one pool.
        while (!name.isEmpty() && (name.back().isDigit() || name.back() == QLatin1Char('-'))) {
            name.chop(1);
        }
        const QStringList rest = line.mid(close + 2).split(QLatin1Char(' '));
        if (rest.size() < 13) continue;
        const int64_t ticks = rest[11].toLongLong() + rest[12].toLongLong();  // utime, stime
        out[name] += ticks * (1'000'000'000 / tick);
    }
    return out;
}

QMap<QString, int64_t> g_threads_at_begin;

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
    const QMap<QString, int64_t> threads_end = threads_cpu_ns();
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
    s << "  per thread (CPU % of one core, from /proc, 10 ms resolution):\n";
    for (auto it = threads_end.begin(); it != threads_end.end(); ++it) {
        const int64_t d = it.value() - g_threads_at_begin.value(it.key(), 0);
        if (d <= 0) continue;
        s << QString::asprintf("    %-20s %6.1f %%\n", qPrintable(it.key()),
                               100.0 * double(d) / double(wall));
    }
    return out;
}

}  // namespace sonora::ui
