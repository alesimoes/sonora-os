#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/stems/separators.h"

using namespace sonora::stems;

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

struct CancelAt : ProgressSink {
    int n = 0, at;
    explicit CancelAt(int a) : at(a) {}
    bool report(double) override { return ++n < at; }
};

int main() {
    const int sr = 44100, N = sr;
    const double w = 2 * 3.14159265358979 / sr;
    std::vector<float> l(N), r(N);
    for (int i = 0; i < N; ++i) {
        float centre = 0.4f * std::sin(w * 440.0 * i);   // identical in L and R
        float side = 0.4f * std::sin(w * 3000.0 * i);    // opposite polarity
        l[i] = centre + side;
        r[i] = centre - side;
    }
    const float* ch[2] = {l.data(), r.data()};
    StemRequest req{ch, 2, N, sr};
    CentreSeparator s;
    StemResult out;
    std::string err;
    CHECK(s.supports(StemKind::Vocals) && s.supports(StemKind::Other));
    CHECK(s.separate(req, out, nullptr, &err));
    const auto& V = out.stems[(int)StemKind::Vocals];
    const auto& O = out.stems[(int)StemKind::Other];
    double ev = 0, eo = 0, sum = 0;
    for (int i = 4096; i < N - 4096; ++i) {
        float c = 0.4f * std::sin(w * 440.0 * i);
        ev += (V[i] - c) * (V[i] - c);
        eo += O[i] * O[i];
        sum = std::fmax(sum, std::fabs(V[i] + O[i] - l[i]));
    }
    std::printf("centre err %g other %g sum err %g\n", ev, eo, sum);
    CHECK(sum < 1e-5);
    CHECK(ev < 0.05 * 0.16 * N / 2);  // centre recovered to > 13 dB
    CHECK(eo > 0.5 * 0.16 * (N - 8192) / 2);  // side content lands in Other

    CentreSeparator mono(1);
    CHECK(!mono.supports(StemKind::Vocals));
    StemRequest mreq{ch, 1, N, sr};
    CHECK(!mono.separate(mreq, out, nullptr, &err));

    CancelAt c(3);
    CHECK(!s.separate(req, out, &c, &err) && err == "cancelled");
    std::puts("ok");
    return 0;
}
