#!/usr/bin/env python3
"""Plots from sonora-dsp-measure's JSON reports (ADR-0011 §3.1).

    tools/dsp-plots.py build-container/dsp-reports docs/images/dsp

Reads <name>.json for the native processors and writes PNGs: the EQ's
measured response against its analog target in both modes, the dynamics
processors' static curves, the reverb's reverberation time per octave
band against its design and its frozen level, the tube's harmonic levels
against ngspice and its aliasing per quality, and the CPU figures
against the class bars.
Needs matplotlib. A reporting aid, not a test: the bars live in each
processor's quality.json and are checked by ctest.
"""
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def load(report_dir, name):
    path = os.path.join(report_dir, name + ".json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def eq_responses(eq, out_dir):
    for key, title in (("response_2x", "analog matched (2x)"), ("response_1x", "zero latency (1x)")):
        curves = eq["data"].get(key, [])
        if not curves:
            continue
        fig, axes = plt.subplots(len(curves), 2, figsize=(12, 2.6 * len(curves)), squeeze=False)
        for row, c in enumerate(curves):
            pts = c["points_hz_db_target"]
            f = [p[0] for p in pts]
            meas = [p[1] for p in pts]
            tgt = [p[2] for p in pts]
            ax = axes[row][0]
            ax.semilogx(f, tgt, color="0.6", lw=3, label="analog target")
            ax.semilogx(f, meas, color="C0", lw=1, label="measured")
            ax.set_ylabel("dB")
            ax.set_title(c["curve"], fontsize=8)
            ax.grid(True, which="both", alpha=0.3)
            if row == 0:
                ax.legend(fontsize=7)
            err = axes[row][1]
            err.semilogx(f, [m - t if t >= -20 else float("nan") for m, t in zip(meas, tgt)], color="C3")
            err.axhline(0.1, color="k", lw=0.5, ls="--")
            err.axhline(-0.1, color="k", lw=0.5, ls="--")
            err.set_ylabel("error, dB")
            err.grid(True, which="both", alpha=0.3)
        axes[-1][0].set_xlabel("Hz")
        axes[-1][1].set_xlabel("Hz (error where the target is above -20 dB; dashed: +-0.1 dB bar)")
        fig.suptitle("Sonora EQ, " + title + ", 48 kHz: measured response against the analog prototype")
        fig.tight_layout(rect=[0, 0, 1, 0.98])
        name = "eq-response-2x.png" if key == "response_2x" else "eq-response-1x.png"
        fig.savefig(os.path.join(out_dir, name), dpi=110)
        plt.close(fig)


def static_curves(comp, gate, out_dir):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    if comp:
        pts = comp["data"].get("static_curve_in_out_formula", [])
        n = 31
        for k in range(len(pts) // n):
            seg = pts[k * n:(k + 1) * n]
            axes[0].plot([p[0] for p in seg], [p[2] for p in seg], color="0.7", lw=3)
            axes[0].plot([p[0] for p in seg], [p[1] for p in seg], lw=1)
        axes[0].set_title("Compressor static curve (3 settings): measured over formula", fontsize=9)
    if gate:
        pts = gate["data"].get("expander_curve_in_out_formula", [])
        axes[1].plot([p[0] for p in pts], [p[2] for p in pts], color="0.7", lw=3, label="formula")
        axes[1].plot([p[0] for p in pts], [p[1] for p in pts], lw=1, label="measured")
        axes[1].legend(fontsize=8)
        axes[1].set_title("Expander (T -40, 1:2, knee 6, range -60): measured over formula", fontsize=9)
    for ax in axes:
        ax.set_xlabel("input, dBFS (DC)")
        ax.set_ylabel("output, dBFS")
        ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "dynamics-static-curves.png"), dpi=110)
    plt.close(fig)


def reverb_plots(rev, out_dir):
    curves = rev["data"].get("t60_bands", [])
    frozen = rev["data"].get("freeze_rms_db_per_second", [])
    if not curves:
        return
    cols = 3
    rows = (len(curves) + 1 + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(13, 3.3 * rows), squeeze=False)
    for k, c in enumerate(curves):
        ax = axes[k // cols][k % cols]
        pts = c["points_hz_measured_design"]
        f = [p[0] for p in pts]
        ax.semilogx(f, [p[2] for p in pts], color="0.6", lw=3, marker="o", label="design T60")
        ax.semilogx(f, [p[1] for p in pts], color="C0", lw=1, marker="x", label="measured T30")
        ax.set_title(c["curve"], fontsize=8)
        ax.set_ylabel("s")
        ax.set_ylim(0, 1.15 * max(max(p[1], p[2]) for p in pts))
        ax.grid(True, which="both", alpha=0.3)
        if k == 0:
            ax.legend(fontsize=7)
    ax = axes[len(curves) // cols][len(curves) % cols]
    if frozen:
        ax.plot([1.5 + i for i in range(len(frozen))], frozen, marker="o")
        ax.set_title("freeze: 1 s RMS of the held tail", fontsize=8)
        ax.set_xlabel("s after the start (frozen at 1 s)")
        ax.set_ylabel("dBFS")
        mid = sum(frozen) / len(frozen)
        ax.set_ylim(mid - 1.0, mid + 1.0)
        ax.grid(True, alpha=0.3)
    for k in range(len(curves) + 1, rows * cols):
        axes[k // cols][k % cols].axis("off")
    for k in range(len(curves)):
        axes[k // cols][k % cols].set_xlabel("octave band, Hz")
    fig.suptitle("Sonora Reverb, 48 kHz: T30 per octave band (ISO 3382-1) against the set decay")
    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig(os.path.join(out_dir, "reverb-t60-bands.png"), dpi=110)
    plt.close(fig)


def tube_plots(tube, out_dir):
    stage = [r for r in tube["data"].get("spice_stage", []) if r["rate"] == 192000.0]
    if stage:
        dem = [r for r in stage if r["case"].startswith("dempwolf")]
        kor = [r for r in stage if r["case"].startswith("koren")]
        rows = [dem[i:i + 4] for i in range(0, len(dem), 4)] + ([kor] if kor else [])
        fig, axes = plt.subplots(len(rows), 4, figsize=(14, 2.7 * len(rows)), squeeze=False)
        for i, row in enumerate(rows):
            for j, r in enumerate(row):
                ax = axes[i][j]
                k = list(range(2, 10))
                ax.bar([x - 0.2 for x in k], r["spice_dbc"][1:], 0.4, color="0.6", label="ngspice")
                ax.bar([x + 0.2 for x in k], r["model_dbc"][1:], 0.4, color="C0", label="model, 192 kHz")
                ax.set_title(r["case"] + " (H1 %.1f V)" % r["h1_v"], fontsize=8)
                ax.set_ylim(-120, 0)
                ax.set_xticks(k)
                ax.set_xlabel("harmonic", fontsize=7)
                ax.set_ylabel("dBc", fontsize=7)
                ax.grid(True, axis="y", alpha=0.3)
                if i == 0 and j == 0:
                    ax.legend(fontsize=7)
            for j in range(len(row), 4):
                axes[i][j].axis("off")
        fig.suptitle("Triode stage: harmonic levels, model (4x at 48 kHz) against ngspice on the same netlist")
        fig.tight_layout(rect=[0, 0, 1, 0.97])
        fig.savefig(os.path.join(out_dir, "tube-harmonics-vs-spice.png"), dpi=110)
        plt.close(fig)
    alias = tube["data"].get("aliasing", [])
    if alias:
        names = ["eco (2x)", "standard (4x)", "high (8x)"]
        levels = ["nominal: default drive, -18 dBFS (held)", "default drive, -6 dBFS", "drive +18 dB, -6 dBFS"]
        rates = sorted({a["rate"] for a in alias})
        fig, axes = plt.subplots(1, len(rates), figsize=(5 * len(rates), 3.8), squeeze=False)
        for c, fs in enumerate(rates):
            ax = axes[0][c]
            for q in range(3):
                for lv, style in ((0, "-"), (1, "--"), (2, ":")):
                    pts = sorted((a["tone"], a["alias_dbc"]) for a in alias
                                 if a["rate"] == fs and a["quality"] == q and a["level"] == lv)
                    if pts:
                        ax.plot([p[0] for p in pts], [p[1] for p in pts], style, color="C%d" % q, marker=".",
                                label=names[q] + ", " + levels[lv] if c == 0 else None)
            ax.axhline(-90, color="k", lw=0.8)
            ax.axhline(-70, color="k", lw=0.8, ls="--")
            ax.set_xscale("log")
            ax.set_title("%g kHz" % (fs / 1000.0), fontsize=9)
            ax.set_xlabel("tone, Hz")
            ax.set_ylabel("non-harmonic energy, dBc")
            ax.set_ylim(-160, 0)
            ax.grid(True, which="both", alpha=0.3)
        axes[0][0].legend(fontsize=5.5, loc="upper left")
        fig.suptitle("Tube drive: aliasing per quality (bars: -90 dBc default, -70 dBc eco, at nominal drive)")
        fig.tight_layout(rect=[0, 0, 1, 0.93])
        fig.savefig(os.path.join(out_dir, "tube-aliasing.png"), dpi=110)
        plt.close(fig)


def cpu(reports, out_dir):
    rows = []
    for name, rep in reports.items():
        if not rep:
            continue
        figs = {}
        for c in rep["checks"]:
            label = c.get("check", "")
            if not label.startswith("CPU per block"):
                continue
            mode = label.split(":")[0].replace("CPU per block, q128 48 kHz stereo", "").strip(", ")
            key = (name + (" " + mode if mode else ""))
            stat = label.split(":")[-1].strip()
            figs.setdefault(key, {})[stat] = (c.get("value"), c.get("bar"))
        for key, v in figs.items():
            rows.append((key, v))
    if not rows:
        return
    fig, ax = plt.subplots(figsize=(10, 4.5))
    labels = [r[0] for r in rows]
    x = range(len(rows))
    p50 = [r[1].get("p50", (0, None))[0] for r in rows]
    p999 = [r[1].get("p99.9", (0, None))[0] for r in rows]
    single = [r[1].get("p99.9, single run", (0, None))[0] for r in rows]
    bars = [r[1].get("p99.9", (0, 0))[1] for r in rows]
    w = 0.27
    ax.bar([i - w for i in x], p50, w, label="p50 (min of 5 runs)")
    ax.bar(list(x), p999, w, label="p99.9 (min of 5 runs)")
    ax.bar([i + w for i in x], single, w, label="p99.9, single run (host jitter)", color="0.75")
    ax.scatter(list(x), bars, marker="_", s=600, color="k", label="class bar, p99.9")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=8)
    ax.set_ylabel("microseconds per 128-frame block")
    ax.set_title("CPU per instance, q128, 48 kHz, stereo (i7-11800H, Docker Desktop on Windows)")
    ax.legend(fontsize=7)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "cpu-per-instance.png"), dpi=110)
    plt.close(fig)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    report_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    names = ["eq", "compressor", "gate", "limiter", "delay", "chorus", "reverb", "tube"]
    reports = {n: load(report_dir, n) for n in names}
    if reports["eq"]:
        eq_responses(reports["eq"], out_dir)
    static_curves(reports["compressor"], reports["gate"], out_dir)
    if reports["reverb"]:
        reverb_plots(reports["reverb"], out_dir)
    if reports["tube"]:
        tube_plots(reports["tube"], out_dir)
    cpu(reports, out_dir)
    print("plots written to", out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
