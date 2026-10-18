// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/measure/spice_fet.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "sonora/dsp/math.h"
#include "spice_util.h"

namespace sonora::measure {

namespace {

using spice_util::jnum;
using spice_util::ngspice_version;
using spice_util::read_file;
using spice_util::run_ngspice;
using spice_util::sp;
using spice_util::write_file;

double amp(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

double get(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    double d = 0.0;
    if (v == nullptr || !v->to_double(d)) return std::nan("");
    return d;
}

// ngspice's time step bound: 200 points a period, so the rectifier's
// conduction at each peak is resolved.
double max_step(const FetSpiceCase& c) { return 1.0 / (200.0 * c.freq_hz); }

double end_s(const FetSpiceCase& c) { return c.quiet_s + c.hot_s + c.release_s; }

// The model at a case's settings, from rest.
void setup(dsp::FetChannel& ch, const FetSpiceCase& c, double rate, const dsp::FetCircuit& k,
           const dsp::JfetParams& j, const dsp::DiodeParams& d) {
    ch.prepare(rate, k, j, d);
    ch.set_ratio(c.ratio);
    ch.set_attack_ohms(c.r55);
    ch.set_release_ohms(c.r56);
    ch.reset();
}

// ngspice's wrdata rows: time v(c) time v(q).
bool read_trajectory(const std::string& path, std::vector<double>& t, std::vector<double>& vc,
                     std::vector<double>& vq, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot read " + path;
        return false;
    }
    double a, b, a2, b2;
    while (f >> a >> b >> a2 >> b2) {
        t.push_back(a);
        vc.push_back(b);
        vq.push_back(b2);
    }
    if (t.size() < 100) {
        error = "ngspice output " + path + " is too short";
        return false;
    }
    return true;
}

std::vector<double> sample_at(const std::vector<double>& t, const std::vector<double>& v,
                              const std::vector<double>& times) {
    std::vector<double> out;
    out.reserve(times.size());
    std::size_t i = 0;
    for (double x : times) {
        while (i + 2 < t.size() && t[i + 1] < x) ++i;
        const double span = t[i + 1] - t[i];
        const double u = span > 0.0 ? (x - t[i]) / span : 0.0;
        out.push_back(v[i] + (v[i + 1] - v[i]) * dsp::clamp(u, 0.0, 1.0));
    }
    return out;
}

}  // namespace

std::vector<FetSpiceCase> fet_spice_cases() {
    // Quiet is -60 dBFS: below every threshold, so the release is the
    // network's own. Levels at the input control's 0 dB. The cases cover
    // each switch position, both ends of the attack and release controls,
    // a 10 kHz burst (the fastest attack, whose first peak arrives in
    // 25 us), 100 Hz (the ripple the release follows), and +18 dBFS (the
    // side chain's swing and the gate's forward limit).
    return {
        {"r4-a1803-r0-1000hz-m6", 0.0, 1803.0, 0.0, 1000.0, -60.0, -6.0, 0.2, 1.0, 2.0},
        {"r20-a0-r866k-10000hz-p6", 3.0, 0.0, 866e3, 10000.0, -60.0, 6.0, 0.05, 0.1, 0.3},
        {"r8-a25k-r5m-1000hz-0", 1.0, 25e3, 5e6, 1000.0, -60.0, 0.0, 0.2, 1.0, 2.0},
        {"r12-a1803-r866k-100hz-0", 2.0, 1803.0, 866e3, 100.0, -60.0, 0.0, 0.2, 1.0, 2.0},
        {"all-a1803-r866k-1000hz-m12", 4.0, 1803.0, 866e3, 1000.0, -60.0, -12.0, 0.2, 1.0, 2.0},
        {"r20-a0-r0-1000hz-p18", 3.0, 0.0, 0.0, 1000.0, -60.0, 18.0, 0.1, 0.5, 1.5},
    };
}

std::vector<double> fet_spice_times(const FetSpiceCase& c) {
    // In units of 10 us, so every grid point is exact.
    const long q_end = std::lround(end_s(c) * 1e5);
    const long changes[] = {std::lround(c.quiet_s * 1e5), std::lround((c.quiet_s + c.hot_s) * 1e5)};
    std::vector<double> t;
    for (long q = 0; q <= q_end;) {
        t.push_back(static_cast<double>(q) * 1e-5);
        long step = 1000;  // 10 ms
        for (long ch : changes) {
            long s = step;
            if (q >= ch && q < ch + 200) s = 1;                 // 2 ms after a step: 10 us
            else if (q >= ch && q < ch + 4000) s = 25;          // to 40 ms: 0.25 ms
            else if (q >= ch && q < ch + 30000) s = 100;        // to 300 ms: 1 ms
            if (s < step) step = s;
            if (q < ch && q + step > ch) step = ch - q;         // land on the step
        }
        q += step;
    }
    return t;
}

double fet_case_input(const FetSpiceCase& c, double t) {
    const double a = (t >= c.quiet_s && t < c.quiet_s + c.hot_s) ? amp(c.hot_dbfs) : amp(c.quiet_dbfs);
    return a * std::sin(dsp::kTwoPi * c.freq_hz * t);
}

double fet_gr_db(const dsp::FetCircuit& k, const dsp::JfetParams& j, double vg0) {
    const double g = 2.0 * j.beta * std::fmax(std::fmin(vg0, k.gate_max_v) - j.vto, 0.0);
    return 20.0 * std::log10(std::pow(10.0, -k.rest_loss_db / 20.0) * (1.0 + k.r5 * g));
}

std::string fet_netlist(const FetSpiceCase& c, const dsp::FetCircuit& k, const dsp::JfetParams& j,
                        const dsp::DiodeParams& d) {
    dsp::FetChannel ch;
    setup(ch, c, 48000.0, k, j, d);
    const double gain = k.sc_gain * ch.tap() * k.preamp_gain;
    const double vq = k.full_scale_v * amp(c.quiet_dbfs), vh = k.full_scale_v * amp(c.hot_dbfs);
    const double t1 = c.quiet_s, t2 = c.quiet_s + c.hot_s, te = end_s(c);
    const double v0 = ch.control_v();
    std::ostringstream n;
    n << "* Sonora SPICE reference: FET gain-reduction stage and side chain, " << c.name << "\n"
      << "* Generated by sonora-dsp-measure --spice-kind fet --spice-generate from common/dsp/fet.h;\n"
      << "* do not edit (plugins/native/measure/include/sonora/measure/spice_fet.h).\n"
      // The tone burst at R5: its amplitude steps on zero crossings.
      << "VAMP amp 0 PWL(0 " << sp(vq) << " " << sp(t1) << " " << sp(vq) << " " << sp(t1 + 1e-7) << " " << sp(vh)
      << " " << sp(t2) << " " << sp(vh) << " " << sp(t2 + 1e-7) << " " << sp(vq) << " " << sp(te) << " " << sp(vq)
      << ")\n"
      << "BIN in 0 V=v(amp)*sin(" << sp(dsp::kTwoPi * c.freq_hz) << "*time)\n"
      // The divider: R5 into the FET, ngspice's NJF level 1 with [J]'s
      // values; its gate junctions off (IS tiny), the gate's forward limit
      // being in the gate's source below.
      << "R5 in d " << sp(k.r5) << "\n"
      << "J1 d g 0 QFET\n"
      << ".model QFET NJF(BETA=" << sp(j.beta) << " VTO=" << sp(j.vto) << " LAMBDA=" << sp(j.lambda)
      << " IS=1e-30)\n"
      // The LN gate: the middle of R7 (from C22) and R6 (from C9, which
      // carries the trimmed preamplifier output, vd at audio).
      << "BG g 0 V=min((v(c)+v(q))/2," << sp(k.gate_max_v) << ")+v(d)/2\n"
      << "RGQ c q " << sp(k.r6 + k.r7) << "\n"
      << "C9 q 0 " << sp(k.c9) << "\n"
      << "C22 c 0 " << sp(k.c22) << "\n"
      // The side chain: the ratio tap, the amplifier and its swing, the
      // phase splitter putting the two anodes at Vb +- s.
      << "BS s 0 V=max(min(" << sp(gain) << "*v(d)," << sp(k.sc_swing_v) << "),-" << sp(k.sc_swing_v) << ")\n"
      << "BA1 a1 0 V=" << sp(ch.bias_v()) << "+v(s)\n"
      << "BA2 a2 0 V=" << sp(ch.bias_v()) << "-v(s)\n"
      << "D2 a1 w QD\n"
      << "D3 a2 w QD\n"
      << ".model QD D(IS=" << sp(d.is) << " N=" << sp(d.n) << ")\n"
      // Attack: R54 || C27, then R55; release: R57 + R56 to the rest point.
      << "R54 w x " << sp(k.r54) << "\n"
      << "C27 w x " << sp(k.c27) << "\n";
    if (c.r55 > 0.0) n << "R55 x c " << sp(c.r55) << "\n";
    else n << "VR55 x c DC 0\n";
    n << "RREL c rest " << sp(k.r57 + c.r56) << "\n"
      << "VREST rest 0 DC " << sp(ch.rest_v()) << "\n"
      << ".ic v(c)=" << sp(v0) << " v(q)=" << sp(v0) << " v(w)=" << sp(v0) << " v(x)=" << sp(v0) << "\n"
      << ".options reltol=1e-7 abstol=1e-15 vntol=1e-12 method=trap\n"
      << ".control\n"
      << "tran " << sp(max_step(c)) << " " << sp(te) << " 0 " << sp(max_step(c)) << " uic\n"
      << "wrdata out.txt v(c) v(q)\n"
      << ".endc\n"
      << ".end\n";
    return n.str();
}

std::vector<double> fet_model_gr(const FetSpiceCase& c, double rate, const std::vector<double>& times,
                                 const dsp::FetCircuit& k, const dsp::JfetParams& j, const dsp::DiodeParams& d) {
    dsp::FetChannel ch;
    setup(ch, c, rate, k, j, d);
    // Sample n solves the circuit at its own instant, n / fs (fet.h, the
    // joint solve), so the state after it is the one at n / fs.
    const auto count = static_cast<std::size_t>(std::ceil(end_s(c) * rate)) + 2;
    std::vector<double> gr(count, 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        ch.tick(k.full_scale_v * fet_case_input(c, static_cast<double>(i) / rate));
        gr[i] = ch.gain_reduction_db();
    }
    std::vector<double> out;
    out.reserve(times.size());
    for (double t : times) {
        const double x = t * rate;
        const auto i = static_cast<std::size_t>(std::floor(x));
        const double u = x - static_cast<double>(i);
        out.push_back(i + 1 < gr.size() ? gr[i] + (gr[i + 1] - gr[i]) * u : gr.back());
    }
    return out;
}

json::Value fet_circuit_json(const dsp::FetCircuit& k) {
    json::Value o = json::Value::object();
    const std::pair<const char*, double> v[] = {
        {"r5", k.r5},           {"r6", k.r6},           {"r7", k.r7},
        {"c9", k.c9},           {"c22", k.c22},         {"r54", k.r54},
        {"c27", k.c27},         {"r55_max", k.r55_max}, {"r57", k.r57},
        {"r56_max", k.r56_max}, {"rs_in", k.rs_in},     {"r19", k.r19},
        {"r20", k.r20},         {"r21", k.r21},         {"r22", k.r22},
        {"r76", k.r76},         {"r75", k.r75},         {"r73", k.r73},
        {"r68", k.r68},         {"r64", k.r64},         {"r35", k.r35},
        {"preamp_gain", k.preamp_gain}, {"rest_loss_db", k.rest_loss_db}, {"sc_gain", k.sc_gain},
        {"bias_drive_v", k.bias_drive_v}, {"sc_swing_v", k.sc_swing_v}, {"gate_max_v", k.gate_max_v},
        {"full_scale_v", k.full_scale_v},
    };
    for (const auto& kv : v) o.set(kv.first, jnum(kv.second));
    return o;
}

json::Value fet_devices_json(const dsp::JfetParams& j, const dsp::DiodeParams& d) {
    json::Value o = json::Value::object();
    const std::pair<const char*, double> v[] = {
        {"jfet_beta", j.beta}, {"jfet_vto", j.vto}, {"jfet_lambda", j.lambda},
        {"diode_is", d.is},    {"diode_n", d.n},    {"diode_vt", d.vt},
    };
    for (const auto& kv : v) o.set(kv.first, jnum(kv.second));
    return o;
}

bool read_fet_spice_reference(const std::string& path, FetSpiceReference& ref, std::string& error) {
    std::string text;
    if (!read_file(path, text)) {
        error = "cannot read " + path;
        return false;
    }
    json::Value root;
    json::Error e;
    if (!json::parse(text, root, e)) {
        error = path + ": " + e.message;
        return false;
    }
    const json::Value* ng = root.find("ngspice");
    const json::Value* cases = root.find("cases");
    const json::Value* circuit = root.find("circuit");
    const json::Value* devices = root.find("devices");
    if (ng == nullptr || cases == nullptr || circuit == nullptr || devices == nullptr || !cases->is_array()) {
        error = path + ": missing fields";
        return false;
    }
    ref.ngspice = ng->text();
    ref.circuit = *circuit;
    ref.devices = *devices;
    for (const json::Value& c : cases->items()) {
        const json::Value* name = c.find("name");
        const json::Value* t = c.find("t_s");
        const json::Value* g = c.find("gr_db");
        if (name == nullptr || t == nullptr || g == nullptr || t->items().size() != g->items().size()) {
            error = path + ": malformed case";
            return false;
        }
        FetSpiceCase fc{name->text(),         get(c, "ratio"),      get(c, "r55"),        get(c, "r56"),
                        get(c, "freq_hz"),    get(c, "quiet_dbfs"), get(c, "hot_dbfs"),   get(c, "quiet_s"),
                        get(c, "hot_s"),      get(c, "release_s")};
        std::vector<double> tv, gv;
        for (const json::Value& x : t->items()) {
            double v = 0.0;
            x.to_double(v);
            tv.push_back(v);
        }
        for (const json::Value& x : g->items()) {
            double v = 0.0;
            x.to_double(v);
            gv.push_back(v);
        }
        ref.cases.push_back(fc);
        ref.times.push_back(std::move(tv));
        ref.gr_db.push_back(std::move(gv));
    }
    return true;
}

namespace {

// One case through ngspice: the gain reduction on the case's grid.
bool spice_gr(const FetSpiceCase& c, const std::string& netlist, const std::string& ngspice, const std::string& work,
              std::vector<double>& gr, std::string& err) {
    const dsp::FetCircuit k;
    const dsp::JfetParams j;
    std::vector<double> t, vc, vq;
    if (!run_ngspice(ngspice, work, netlist, err) || !read_trajectory(work + "/out.txt", t, vc, vq, err)) return false;
    const std::vector<double> times = fet_spice_times(c);
    const std::vector<double> a = sample_at(t, vc, times), b = sample_at(t, vq, times);
    gr.clear();
    for (std::size_t i = 0; i < times.size(); ++i) gr.push_back(fet_gr_db(k, j, 0.5 * (a[i] + b[i])));
    return true;
}

}  // namespace

int fet_spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work) {
    const dsp::FetCircuit k;
    const dsp::JfetParams j;
    const dsp::DiodeParams d;
    json::Value root = json::Value::object();
    root.set("generator", json::Value::string("sonora-dsp-measure --spice-kind fet --spice-generate"));
    root.set("ngspice", json::Value::string(ngspice_version(ngspice, work)));
    root.set("circuit", fet_circuit_json(k));
    root.set("devices", fet_devices_json(j, d));
    json::Value cases = json::Value::array();
    for (const FetSpiceCase& c : fet_spice_cases()) {
        const std::string netlist = fet_netlist(c, k, j, d);
        if (!write_file(dir + "/" + c.name + ".cir", netlist)) {
            std::fprintf(stderr, "cannot write %s/%s.cir\n", dir.c_str(), c.name.c_str());
            return 1;
        }
        std::vector<double> gr;
        std::string err;
        if (!spice_gr(c, netlist, ngspice, work, gr, err)) {
            std::fprintf(stderr, "%s: %s\n", c.name.c_str(), err.c_str());
            return 1;
        }
        json::Value o = json::Value::object();
        o.set("name", json::Value::string(c.name));
        o.set("ratio", jnum(c.ratio));
        o.set("r55", jnum(c.r55));
        o.set("r56", jnum(c.r56));
        o.set("freq_hz", jnum(c.freq_hz));
        o.set("quiet_dbfs", jnum(c.quiet_dbfs));
        o.set("hot_dbfs", jnum(c.hot_dbfs));
        o.set("quiet_s", jnum(c.quiet_s));
        o.set("hot_s", jnum(c.hot_s));
        o.set("release_s", jnum(c.release_s));
        json::Value tj = json::Value::array(), gj = json::Value::array();
        const std::vector<double> times = fet_spice_times(c);
        double peak = 0.0;
        for (std::size_t i = 0; i < times.size(); ++i) {
            tj.push(jnum(times[i]));
            // Nine significant digits: far below any bar, and a file a
            // reviewer can open.
            char b[32];
            std::snprintf(b, sizeof b, "%.9g", gr[i]);
            gj.push(json::Value::number_lexeme(b));
            peak = std::fmax(peak, gr[i]);
        }
        o.set("t_s", tj);
        o.set("gr_db", gj);
        cases.push(o);
        std::printf("  %-34s %zu points, deepest reduction %.3f dB\n", c.name.c_str(), times.size(), peak);
    }
    root.set("cases", cases);
    std::string text, err;
    if (!json::write(root, text, err) || !write_file(dir + "/reference.json", text + "\n")) {
        std::fprintf(stderr, "cannot write %s/reference.json: %s\n", dir.c_str(), err.c_str());
        return 1;
    }
    return 0;
}

int fet_spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                    int expected_cases) {
    FetSpiceReference ref;
    std::string err;
    if (!read_fet_spice_reference(dir + "/reference.json", ref, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const auto cases = fet_spice_cases();
    if (expected_cases != static_cast<int>(cases.size())) {
        std::fprintf(stderr, "the build registers %d spice_fet_* tests, the harness has %zu cases; update "
                     "plugins/native/fet/CMakeLists.txt\n", expected_cases, cases.size());
        return 1;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= cases.size() || ref.cases.size() != cases.size()) {
        std::fprintf(stderr, "case %d: the reference has %zu cases, the harness %zu\n", index, ref.cases.size(),
                     cases.size());
        return 1;
    }
    const FetSpiceCase& c = cases[static_cast<std::size_t>(index)];
    int bad = 0;
    const std::string netlist = fet_netlist(c, dsp::FetCircuit{}, dsp::JfetParams{}, dsp::DiodeParams{});
    std::string committed;
    if (!read_file(dir + "/" + c.name + ".cir", committed) || committed != netlist) {
        std::fprintf(stderr, "FAIL %s.cir differs from the netlist the model's values generate; regenerate the "
                     "references (--spice-kind fet --spice-generate) in a deliberate commit\n", c.name.c_str());
        ++bad;
    }
    std::vector<double> gr;
    if (!spice_gr(c, netlist, ngspice, work, gr, err)) {
        std::fprintf(stderr, "FAIL %s: %s\n", c.name.c_str(), err.c_str());
        return 1;
    }
    const std::vector<double>& r = ref.gr_db[static_cast<std::size_t>(index)];
    double worst = 0.0;
    for (std::size_t i = 0; i < r.size() && i < gr.size(); ++i) worst = std::fmax(worst, std::fabs(gr[i] - r[i]));
    // ngspice is deterministic on one build; another version may move the
    // last digits. 1e-5 dB is far below the model's bar.
    std::printf("%s: %s now vs the committed reference (%s): max |dGR| = %.3g dB (bar 1e-5)\n", c.name.c_str(),
                ngspice_version(ngspice, work).c_str(), ref.ngspice.c_str(), worst);
    if (!(worst <= 1e-5) || gr.size() != r.size()) {
        std::fprintf(stderr, "FAIL %s: the trajectory moved\n", c.name.c_str());
        ++bad;
    }
    return bad == 0 ? 0 : 1;
}

}  // namespace sonora::measure
