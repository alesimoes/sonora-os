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
    const int sr = 44100, N = sr;  // 1 s
    std::vector<float> x(N);
    for (int i = 0; i < N; ++i) x[i] = 0.5f * std::sin(2 * 3.14159265358979 * 440.0 * i / sr);
    for (int i = 4000; i < N; i += 8000) x[i] += 1.0f;  // clicks
    const float* ch[1] = {x.data()};
    StemRequest req{ch, 1, N, sr};

    HpssSeparator s;
    CHECK(s.supports(StemKind::Harmonic) && s.supports(StemKind::Percussive));
    CHECK(!s.supports(StemKind::Vocals));
    StemResult out;
    std::string err;
    CHECK(s.separate(req, out, nullptr, &err));
    const auto& H = out.stems[(int)StemKind::Harmonic];
    const auto& P = out.stems[(int)StemKind::Percussive];
    CHECK((int)H.size() == N && (int)P.size() == N);

    double maxerr = 0;
    for (int i = 0; i < N; ++i) maxerr = std::fmax(maxerr, std::fabs(H[i] + P[i] - x[i]));
    std::printf("reconstruction max error %g\n", maxerr);
    CHECK(maxerr < 1e-4);

    // Click energy goes to percussive; the sine stays harmonic.
    double click_p = 0, click_h = 0, sine_h = 0, sine_p = 0;
    for (int i = 4000; i < N; i += 8000)
        for (int j = -4; j <= 4; ++j) {
            // Subtract the sine so only the click's share is compared.
            float sine = 0.5f * std::sin(2 * 3.14159265358979 * 440.0 * (i + j) / sr);
            float hp = H[i + j] - sine;
            click_p += P[i + j] * P[i + j];
            click_h += hp * hp;
        }
    for (int i = 8000; i < N - 8000; ++i) {
        sine_h += H[i] * H[i];
        sine_p += P[i] * P[i];
    }
    std::printf("click P/H %g sine H/P %g\n", click_p / click_h, sine_h / sine_p);
    CHECK(click_p > 2 * click_h);  // percussive carries most of the click
    CHECK(sine_h > 5 * sine_p);

    CancelAt c(3);
    CHECK(!s.separate(req, out, &c, &err) && err == "cancelled");
    CHECK(!s.separate(StemRequest{}, out, nullptr, &err) && err == "bad request");
    CHECK(default_separators().size() == 2);
    std::puts("ok");
    return 0;
}
