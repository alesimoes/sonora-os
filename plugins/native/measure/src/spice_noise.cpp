// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/measure/spice_noise.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "sonora/dsp/math.h"
#include "sonora/measure/spice.h"
#include "spice_util.h"

namespace sonora::measure {

namespace {

using spice_util::jnum;
using spice_util::ngspice_version;
using spice_util::read_file;
using spice_util::run_ngspice;
using spice_util::sp;
using spice_util::write_file;

const char* kind_name(NoiseSpiceKind k) {
    return k == NoiseSpiceKind::Noise ? "noise" : (k == NoiseSpiceKind::Supply ? "supply" : "chain");
}

double get(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    double d = 0.0;
    if (v == nullptr || !v->to_double(d)) return std::nan("");
    return d;
}

std::vector<double> numbers(const json::Value* a) {
    std::vector<double> out;
    if (a == nullptr) return out;
    for (const json::Value& x : a->items()) {
        double d = 0.0;
        x.to_double(d);
        out.push_back(d);
    }
    return out;
}

json::Value array(const std::vector<double>& v) {
    json::Value a = json::Value::array();
    for (double x : v) a.push(jnum(x));
    return a;
}

double ripple_step(const NoiseSpiceCase& c) { return 1.0 / (2000.0 * 2.0 * c.mains_hz); }

// The stage, as spice.cpp writes it for the Dempwolf law, with the device
// laws reading the grid at `grid` (g, or gi behind the shot-noise source).
void stage_lines(std::ostringstream& n, const dsp::TriodeStageCircuit& k, const dsp::TriodeDevice& d,
                 const char* supply_node, const char* grid) {
    const std::string vgk = std::string("v(") + grid + ",k)";
    n << "CIN in a " << sp(k.c_in) << "\n"
      << "RLEAK a 0 " << sp(k.r_leak) << "\n"
      << "RGRID a g " << sp(k.r_grid) << "\n"
      << "RK k 0 " << sp(k.r_cathode) << "\n"
      << "CK k 0 " << sp(k.c_cathode) << "\n"
      << "RP " << supply_node << " p " << sp(k.r_plate) << "\n"
      << "COUT p o " << sp(k.c_out) << "\n"
      << "RL o 0 " << sp(k.r_load) << "\n"
      << "CGP g p " << sp(d.c_gp) << "\n";
    // Dempwolf and Zolzer, DAFx-11, eqs. (10)-(12).
    const std::string ig = sp(d.gg) + "*pow(ln(1+exp(" + sp(d.cg) + "*" + vgk + "))/" + sp(d.cg) + "," + sp(d.xi) +
                           ")+" + sp(d.ig0);
    n << "BIP p k I=" << sp(d.g) << "*pow(ln(1+exp(" << sp(d.c) << "*(v(p,k)/" << sp(d.mu) << "+" << vgk << ")))/"
      << sp(d.c) << "," << sp(d.gamma) << ")-(" << ig << ")\n"
      << "BIG g k I=" << ig << "\n";
}

dsp::TriodeStage model_stage(const dsp::TriodeStageCircuit& k, const dsp::TriodeDevice& d) {
    dsp::TriodeStage st;
    st.prepare(192000.0, dsp::TriodeLaw::Dempwolf, k, d);
    return st;
}

bool read_rows(const std::string& path, std::size_t cols, std::vector<std::vector<double>>& rows, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot read " + path;
        return false;
    }
    rows.clear();
    std::vector<double> r(cols);
    for (;;) {
        for (std::size_t c = 0; c < cols; ++c) {
            if (f >> r[c]) continue;
            if (rows.empty()) error = path + ": no data";
            return !rows.empty();
        }
        rows.push_back(r);
    }
}

bool run_case(const NoiseSpiceCase& c, const NoiseSpiceTiming& t, const std::string& ngspice, const std::string& work,
              const std::string& netlist, NoiseSpiceResult& out, std::string& error) {
    out = NoiseSpiceResult{};
    if (!run_ngspice(ngspice, work, netlist, error)) return false;
    std::vector<std::vector<double>> rows;
    if (c.kind == NoiseSpiceKind::Noise) {
        if (!read_rows(work + "/out.txt", 2, rows, error)) return false;
        for (const auto& r : rows) {
            out.freq.push_back(r[0]);
            out.density.push_back(r[1]);
        }
        return true;
    }
    if (c.kind == NoiseSpiceKind::Chain) {
        if (!read_rows(work + "/out.txt", 3, rows, error)) return false;
        for (const auto& r : rows) {
            out.freq.push_back(r[0]);
            out.value.emplace_back(r[1], r[2]);
        }
        return true;
    }
    Harmonics h;
    if (!spice_harmonics(work + "/out.txt", 2.0 * c.mains_hz, t.settle_s, t.settle_s + t.window_s, h, error))
        return false;
    out.value = h;
    return true;
}

}  // namespace

std::vector<NoiseSpiceCase> noise_spice_cases() {
    return {
        {"noise", NoiseSpiceKind::Noise, 0.0},
        {"supply-50hz", NoiseSpiceKind::Supply, 50.0},
        {"supply-60hz", NoiseSpiceKind::Supply, 60.0},
        {"chain-50hz", NoiseSpiceKind::Chain, 50.0},
        {"chain-60hz", NoiseSpiceKind::Chain, 60.0},
    };
}

std::string noise_spice_netlist(const NoiseSpiceCase& c, const NoiseSpiceTiming& t, const dsp::TriodeStageCircuit& k,
                                const dsp::TriodeDevice& d, const dsp::SupplyCircuit& supply) {
    std::ostringstream n;
    n << "* Sonora SPICE reference: tube stage noise and hum, " << c.name << "\n"
      << "* Generated by sonora-dsp-measure --spice-generate --spice-kind noise from common/dsp\n"
      << "* triode.h and supply.h; do not edit (plugins/native/measure/include/sonora/measure/spice_noise.h).\n";
    const dsp::TriodeStage st = model_stage(k, d);
    if (c.kind == NoiseSpiceKind::Noise) {
        n << "VIN in 0 DC 0 AC 1\n"
          << "VBP bp 0 DC " << sp(k.supply_v) << "\n";
        stage_lines(n, k, d, "bp", "gi");
        // Shot noise: Req = 2.5 / gm at the model's operating point; its
        // thermal voltage, unloaded, is 4kT Req V^2/Hz.
        n << "RSHOT ns 0 " << sp(st.shot_noise_resistance()) << "\n"
          << "ESHOT gi g ns 0 1\n"
          << ".options reltol=1e-7 abstol=1e-10 vntol=1e-10\n"
          << ".control\n"
          << "op\n"
          << "noise v(o) VIN dec 20 10 40000\n"
          << "setplot noise1\n"
          << "wrdata out.txt onoise_spectrum\n"
          << ".endc\n"
          << ".end\n";
        return n.str();
    }
    dsp::SupplyCircuit s = supply;
    s.mains_hz = c.mains_hz;
    if (c.kind == NoiseSpiceKind::Supply) {
        // Start each capacitor at the DC the model's integration settles
        // to, so the filter's slow mode is not excited; the ripple is the
        // periodic steady state whatever the start.
        dsp::SupplyRipple r;
        dsp::simulate_supply(s, 1, r);
        const double v1 = r.reservoir_dc, v2 = v1 - s.r_filter1 * s.i_preamp, v3 = r.node_dc;
        const double step = ripple_step(c);
        n << "VS s1 s2 SIN(0 " << sp(s.secondary_v_peak) << " " << sp(s.mains_hz) << ")\n"
          << "RW s1 s1r " << sp(s.r_winding) << "\n"
          // The secondary floats; this gives it a DC reference.
          << "RFL s2 0 1e9\n"
          << "D1 s1r res DR\n"
          << "D2 s2 res DR\n"
          << "D3 0 s1r DR\n"
          << "D4 0 s2 DR\n"
          << ".model DR D(IS=" << sp(s.diode_is) << " N=" << sp(s.diode_n) << ")\n"
          << "C1 res 0 " << sp(s.c_reservoir) << " IC=" << sp(v1) << "\n"
          << "IOUT res 0 DC " << sp(s.i_output) << "\n"
          << "R1 res n1 " << sp(s.r_filter1) << "\n"
          << "C2 n1 0 " << sp(s.c_filter1) << " IC=" << sp(v2) << "\n"
          << "R2 n1 node " << sp(s.r_filter2) << "\n"
          << "C3 node 0 " << sp(s.c_filter2) << " IC=" << sp(v3) << "\n"
          << "IPRE node 0 DC " << sp(s.i_preamp) << "\n"
          << ".options reltol=1e-7 abstol=1e-10 vntol=1e-10 method=trap\n"
          << ".control\n"
          << "tran " << sp(step) << " " << sp(t.settle_s + t.window_s) << " " << sp(t.settle_s - 2.0 * step) << " "
          << sp(step) << " uic\n"
          << "wrdata out.txt v(res)\n"
          << ".endc\n"
          << ".end\n";
        return n.str();
    }
    // Chain: the reservoir's DC puts the stage's B+ at the model's supply
    // voltage; the rest of the preamplifier's draw is a constant current.
    const double ripple_hz = 2.0 * s.mains_hz;
    n << "VRES res 0 DC " << sp(k.supply_v + (s.r_filter1 + s.r_filter2) * s.i_preamp) << " AC 1\n"
      << "R1 res n1 " << sp(s.r_filter1) << "\n"
      << "C2 n1 0 " << sp(s.c_filter1) << "\n"
      << "R2 n1 bp " << sp(s.r_filter2) << "\n"
      << "C3 bp 0 " << sp(s.c_filter2) << "\n"
      << "IOTH bp 0 DC " << sp(s.i_preamp - st.op_plate_current()) << "\n"
      << "VIN in 0 DC 0\n";
    stage_lines(n, k, d, "bp", "g");
    n << ".options reltol=1e-7 abstol=1e-10 vntol=1e-10\n"
      << ".control\n"
      << "op\n"
      << "ac lin " << kSpiceHarmonics << " " << sp(ripple_hz) << " " << sp(ripple_hz * kSpiceHarmonics) << "\n"
      << "wrdata out.txt v(o)\n"
      << ".endc\n"
      << ".end\n";
    return n.str();
}

double model_noise_density(const dsp::TriodeStage& st, double f_hz) {
    double s[dsp::TriodeStage::kNoiseSources];
    st.small_signal_noise(f_hz, s);
    double sum = 0.0;
    for (double x : s) sum += x;
    return std::sqrt(sum);
}

std::complex<double> model_chain_transfer(const dsp::TriodeStage& st, const dsp::SupplyCircuit& s, double f_hz) {
    std::complex<double> gain, admittance;
    st.small_signal_supply(f_hz, gain, admittance);
    return dsp::supply_filter_transfer(s, f_hz, admittance) * gain;
}

json::Value supply_json(const dsp::SupplyCircuit& s) {
    json::Value o = json::Value::object();
    const std::pair<const char*, double> v[] = {
        {"secondary_v_peak", s.secondary_v_peak}, {"r_winding", s.r_winding}, {"diode_is", s.diode_is},
        {"diode_n", s.diode_n},                   {"c_reservoir", s.c_reservoir}, {"i_output", s.i_output},
        {"r_filter1", s.r_filter1},               {"c_filter1", s.c_filter1}, {"r_filter2", s.r_filter2},
        {"c_filter2", s.c_filter2},               {"i_preamp", s.i_preamp},
    };
    for (const auto& kv : v) o.set(kv.first, jnum(kv.second));
    return o;
}

bool read_noise_spice_reference(const std::string& path, NoiseSpiceReference& ref, std::string& error) {
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
    const json::Value* timing = root.find("timing");
    const json::Value* cases = root.find("cases");
    const json::Value* circuit = root.find("circuit");
    const json::Value* device = root.find("device");
    const json::Value* supply = root.find("supply");
    if (ng == nullptr || timing == nullptr || cases == nullptr || circuit == nullptr || device == nullptr ||
        supply == nullptr || !cases->is_array()) {
        error = path + ": missing fields";
        return false;
    }
    ref.ngspice = ng->text();
    ref.timing.settle_s = get(*timing, "settle_s");
    ref.timing.window_s = get(*timing, "window_s");
    ref.circuit = *circuit;
    ref.device = *device;
    ref.supply = *supply;
    for (const json::Value& c : cases->items()) {
        const json::Value* name = c.find("name");
        const json::Value* kind = c.find("kind");
        if (name == nullptr || kind == nullptr) {
            error = path + ": malformed case";
            return false;
        }
        NoiseSpiceCase sc{name->text(),
                          kind->text() == "noise" ? NoiseSpiceKind::Noise
                                                  : (kind->text() == "supply" ? NoiseSpiceKind::Supply
                                                                              : NoiseSpiceKind::Chain),
                          get(c, "mains_hz")};
        NoiseSpiceResult r;
        r.freq = numbers(c.find("freq"));
        r.density = numbers(c.find("density"));
        const std::vector<double> re = numbers(c.find("re")), im = numbers(c.find("im"));
        for (std::size_t i = 0; i < re.size() && i < im.size(); ++i) r.value.emplace_back(re[i], im[i]);
        const bool ok = sc.kind == NoiseSpiceKind::Noise
                            ? (!r.freq.empty() && r.freq.size() == r.density.size())
                            : (sc.kind == NoiseSpiceKind::Supply ? r.value.size() == kSpiceHarmonics
                                                                 : r.value.size() == kSpiceHarmonics &&
                                                                       r.freq.size() == kSpiceHarmonics);
        if (!ok) {
            error = path + ": malformed case " + sc.name;
            return false;
        }
        ref.cases.push_back(sc);
        ref.results.push_back(r);
    }
    return true;
}

int noise_spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work) {
    const dsp::TriodeStageCircuit circuit;
    const dsp::TriodeDevice device;
    const dsp::SupplyCircuit supply;
    const NoiseSpiceTiming timing;
    const dsp::TriodeStage st = model_stage(circuit, device);
    json::Value root = json::Value::object();
    root.set("generator", json::Value::string("sonora-dsp-measure --spice-generate --spice-kind noise"));
    root.set("ngspice", json::Value::string(ngspice_version(ngspice, work)));
    json::Value tj = json::Value::object();
    tj.set("settle_s", jnum(timing.settle_s));
    tj.set("window_s", jnum(timing.window_s));
    root.set("timing", tj);
    root.set("circuit", circuit_json(circuit));
    root.set("device", device_json(device));
    root.set("supply", supply_json(supply));
    json::Value cases = json::Value::array();
    for (const NoiseSpiceCase& c : noise_spice_cases()) {
        const std::string netlist = noise_spice_netlist(c, timing, circuit, device, supply);
        if (!write_file(dir + "/" + c.name + ".cir", netlist)) {
            std::fprintf(stderr, "cannot write %s/%s.cir\n", dir.c_str(), c.name.c_str());
            return 1;
        }
        NoiseSpiceResult r;
        std::string err;
        if (!run_case(c, timing, ngspice, work, netlist, r, err)) {
            std::fprintf(stderr, "%s: %s\n", c.name.c_str(), err.c_str());
            return 1;
        }
        json::Value o = json::Value::object();
        o.set("name", json::Value::string(c.name));
        o.set("kind", json::Value::string(kind_name(c.kind)));
        o.set("mains_hz", jnum(c.mains_hz));
        if (!r.freq.empty()) o.set("freq", array(r.freq));
        if (!r.density.empty()) o.set("density", array(r.density));
        if (!r.value.empty()) {
            std::vector<double> re, im;
            for (const auto& z : r.value) {
                re.push_back(z.real());
                im.push_back(z.imag());
            }
            o.set("re", array(re));
            o.set("im", array(im));
        }
        cases.push(o);
        if (c.kind == NoiseSpiceKind::Noise) {
            for (std::size_t i = 0; i < r.freq.size(); i += 20)
                std::printf("  %-12s %8.1f Hz: %.4e V/rtHz (model %.4e)\n", c.name.c_str(), r.freq[i], r.density[i],
                            model_noise_density(st, r.freq[i]));
        } else if (c.kind == NoiseSpiceKind::Supply) {
            dsp::SupplyCircuit s = supply;
            s.mains_hz = c.mains_hz;
            dsp::SupplyRipple m;
            dsp::simulate_supply(s, kSpiceHarmonics, m);
            for (std::uint32_t k = 0; k < 3; ++k)
                std::printf("  %-12s H%u %.6f V (model %.6f V)\n", c.name.c_str(), k + 1, std::abs(r.value[k]),
                            std::abs(m.reservoir[k]));
        } else {
            dsp::SupplyCircuit s = supply;
            s.mains_hz = c.mains_hz;
            for (std::size_t i = 0; i < 3; ++i)
                std::printf("  %-12s %6.0f Hz: |T| %.6e (model %.6e)\n", c.name.c_str(), r.freq[i],
                            std::abs(r.value[i]), std::abs(model_chain_transfer(st, s, r.freq[i])));
        }
    }
    root.set("cases", cases);
    std::string text, err;
    if (!json::write(root, text, err) || !write_file(dir + "/reference.json", text + "\n")) {
        std::fprintf(stderr, "cannot write %s/reference.json: %s\n", dir.c_str(), err.c_str());
        return 1;
    }
    return 0;
}

int noise_spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                      int expected_cases) {
    NoiseSpiceReference ref;
    std::string err;
    if (!read_noise_spice_reference(dir + "/reference.json", ref, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const auto cases = noise_spice_cases();
    if (expected_cases != static_cast<int>(cases.size())) {
        std::fprintf(stderr, "the build registers %d spice_noise_* tests, the harness has %zu cases; update "
                     "plugins/native/tube/CMakeLists.txt\n", expected_cases, cases.size());
        return 1;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= cases.size() || ref.cases.size() != cases.size()) {
        std::fprintf(stderr, "case %d: the reference has %zu cases, the harness %zu\n", index, ref.cases.size(),
                     cases.size());
        return 1;
    }
    const NoiseSpiceCase& c = cases[static_cast<std::size_t>(index)];
    int bad = 0;
    const std::string netlist =
        noise_spice_netlist(c, ref.timing, dsp::TriodeStageCircuit{}, dsp::TriodeDevice{}, dsp::SupplyCircuit{});
    std::string committed;
    if (!read_file(dir + "/" + c.name + ".cir", committed) || committed != netlist) {
        std::fprintf(stderr, "FAIL %s.cir differs from the netlist the model's values generate; regenerate the "
                     "references (--spice-generate --spice-kind noise) in a deliberate commit\n", c.name.c_str());
        ++bad;
    }
    NoiseSpiceResult r;
    if (!run_case(c, ref.timing, ngspice, work, netlist, r, err)) {
        std::fprintf(stderr, "FAIL %s: %s\n", c.name.c_str(), err.c_str());
        return 1;
    }
    // ngspice is deterministic on one build; a different version may move
    // the last digits: 1e-6 relative (-120 dB).
    const NoiseSpiceResult& w = ref.results[static_cast<std::size_t>(index)];
    double rel = 0.0;
    if (c.kind == NoiseSpiceKind::Noise) {
        if (r.density.size() != w.density.size()) rel = 1.0;
        for (std::size_t i = 0; rel < 1.0 && i < r.density.size(); ++i)
            rel = std::fmax(rel, std::fabs(r.density[i] - w.density[i]) / w.density[i]);
    } else {
        double scale = 0.0;
        for (const auto& z : w.value) scale = std::fmax(scale, std::abs(z));
        if (r.value.size() != w.value.size()) rel = 1.0;
        for (std::size_t i = 0; rel < 1.0 && i < r.value.size(); ++i)
            rel = std::fmax(rel, std::abs(r.value[i] - w.value[i]) / scale);
    }
    std::printf("%s: %s now vs the committed reference (%s): max relative difference %.3g (bar 1e-6)\n",
                c.name.c_str(), ngspice_version(ngspice, work).c_str(), ref.ngspice.c_str(), rel);
    if (!(rel <= 1e-6)) {
        std::fprintf(stderr, "FAIL %s: the reference moved\n", c.name.c_str());
        ++bad;
    }
    return bad == 0 ? 0 : 1;
}

}  // namespace sonora::measure
