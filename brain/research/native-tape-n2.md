# Tape (ADR-0011 N2 slice 4): sources, measured bars, open defects

`org.sonora.tape`, DSP in `common/dsp/include/sonora/dsp/tape.h`, processor in
`plugins/native/tape`. Status: installed. Eco holds CPU class B (see "CPU
class B"); until then it was EXPERIMENTAL, built and tested but not installed.

## Sources

- Jiles and Atherton, J. Magn. Magn. Mater. 61, 1986; Jiles, IEEE Trans.
  Magn. 28(1), 1992 (parameters, the `max(0, .)` guard with Thoelke and Devine).
- Chowdhury, "Real-time physical modelling for analog tape machines", DAFx-19, 2019
  (per-sample integration, sign of the field step as delta).
- Westmijze 1953 and Mallinson, The Magnetic Recording Handbook, 1990 (bias:
  the anhysteretic regime; head bump at f = v / lambda).
- Wallace, Bell Syst. Tech. J. 30, 1951 (gap and spacing loss).
- NAB 50 us; IEC 70 us (7.5 in/s) and 35 us (15 in/s); 17.5 us at 30 in/s.
- Parameter values (k, c, alpha, gap 2 um, spacing 0.5 um, bump wavelength
  5 mm) are modelling values chosen for the regime, not measured on a machine
  and not fitted to any product.

## Bars, from the first measured run (GCC, Docker, i7-11800H, host loaded)

| Bar | Value | Measured |
|---|---|---|
| reference chain (float vs double, hundreds of substeps) | 1e-4 FS | worst 6.8e-5 (eco, 8 kHz); since "CPU class B": 7.8e-5 (eco, 8 kHz), standard and high under 1.1e-6 |
| the same, eco at 15 kHz, -18 dBFS, drive 18 dB (since "CPU class B") | 1.5e-3 FS | 7.3e-4 |
| head response error, 50 Hz-20 kHz | 0.6 dB | 0.51 dB (7.5 in/s, 44.1 kHz, 20 kHz) |
| aliasing, tones to 8 kHz, drive 12 dB, -12 dBFS | eco -40, standard -80, high -95 dBc | -42.9, -84.3, -98.8 (worst, 44.1 kHz) |
| latency | exact | oversampler + motion line; 30 in/s, bump off, wow/flutter 0 |
| peak speed deviation | 5 % | within 0.01 % at all rates after the delay-line size fix |

Latency is checked with the head-loss low-pass and bump at their least
group delay: they have about 90 us at 7.5 in/s, which a noise correlation
reads as samples; default wow and flutter move the delay by design.

## Aliasing above 8 kHz (not barred, a defect)

Record pre-emphasis lifts 15-19 kHz by 12-17 dB before the curve. Worst
non-harmonic, 19 kHz tone, -12 dBFS, drive 12 dB, dBc:

| Quality | 44.1 kHz | 48 kHz | 96 kHz |
|---|---|---|---|
| eco | -11.2 | -4.2 | -36.8 |
| standard | -22.6 | -36.8 | -61.7 |
| high | -52.3 | -61.7 | -89.5 |

## CPU, first run (p99.9 per 128-frame block, 48 kHz stereo, 20 s, code path)

| Setting | p50 | p99.9 |
|---|---|---|
| eco | 892 us | 1990 us |
| standard | 723 us | 1213 us |
| high | 597 us | 1051 us |
| eco, +12 dB programme, drive 24 dB, motion on | 6062 us | 6469 us |

Class B asks 60 us. The cost was the RK4 step (up to 32 substeps, two tanh
per slope, four slopes) per oversampled sample.

## CPU class B (2026-11-02)

**The integrator.** `TapeHysteresis::tick()` no longer runs RK4. The
Jiles-Atherton ODE is the same as the differential-algebraic form
He = H + alpha M, M = c Man(He) + (1 - c) Mirr, with Mirr a first-order
relaxation towards Man(He) over the distance He travels (k its length),
held while it leads. That relaxation is linear in Mirr, so over a step it
is integrated exactly for Man taken as a polynomial in the distance:
exponential time differencing (Cox and Matthews, J. Comput. Phys. 176,
2002; Hochbruck and Ostermann, Acta Numerica 19, 2010), in He rather than
in time. The polynomial is the quartic through Man and its slope at both
ends and Man halfway. Mirr comes out as a weighted mean of its start and of
Man, so no step size makes it unstable. The end field is implicit: one
Newton step from the last step's susceptibility, applied to first order.
One evaluation per substep: two table lookups, one e^-x, two divisions.
RK4 at 512 substeps with the exact Langevin function stays, as
`tick_with()`, the reference the tests hold this to.

**The table.** L(x) on [0, 16] in 256 cubic Hermite cells (value and slope
continuous), odd by mirroring, 1 - 1/x beyond. Error against `langevin()`
over 2e6 points of [-20, 20]: value 5.2e-9, slope 2.6e-7 (held to 1e-8 and
5e-7 by `test_dsp_tape`). e^-x is written out (Cody-Waite, Taylor to r^8:
relative error 2.7e-10, held to 5e-10), so two lanes compute it at
once.

**Substeps.** One per oversampled sample in eco, up to four in standard and
high (one per unit of a the field moves). Accuracy against 512 RK4
substeps, a sine at 96 kHz, worst |dM| in Ms (`test_dsp_tape` holds
these):

| Case (f, H amplitude in a) | 1 substep | 2 | 4 | 16 |
|---|---|---|---|---|
| 1 kHz, 2 | 8.5e-9 | 8.5e-9 | 8.5e-9 | 8.5e-9 |
| 8 kHz, 5 (the suite's 8 kHz reference tone) | 1.5e-4 | 1.2e-5 | 1.7e-6 | 1.7e-6 |
| 15 kHz, 8 (the suite's 15 kHz reference tone) | 2.7e-3 | 3.4e-4 | 4.2e-5 | 4.1e-6 |
| 1 kHz, 40 | 1.8e-4 | 1.4e-5 | 1.8e-6 | 1.8e-6 |

A cubic in place of the quartic was 4x less accurate at one substep (6.1e-4
on the 8 kHz case) for the same cost: the halfway lookup does not wait on
the end one.

**Two channels in lockstep.** The substep is written once over a lane type
(`src/tape_solve.h`, as `triode_solve.h` is): `tick()`/`run()` with one
double, `tape_run_pair()` with SSE2 on x86-64 (`src/arch/x86_64/`) and two
doubles elsewhere (`src/arch/generic/`). Each lane has the bits the
one-channel call gives (`test_dsp_tape`, `test_dsp_tape_generic`, 40000
samples per cap, 0 mismatches). The block form keeps the state in
registers across a block.

**Why eco takes one substep.** The per-substep cost is a dependent chain,
and a hot input (every sample moving the field more than a) pays the cap
on every sample. Measured on the pair alone, eco-like fields, loaded host
(reference kernel 83.4 us, 2.56x the reference machine's): one substep
54-60 us per block, two 84-113 us. With the processor's own 15-21 us
(oversampler, filters, motion), two substeps put the hot case at about
65 us reference-equivalent, over the bar; one leaves it at 54. The eco
cost of that is the 15 kHz reference error, 7.3e-4 FS (bar 1.5e-3, its own:
`reference.eco_hf_max_error`), which is under eco's own aliasing at 15-19
kHz (-4 to -37 dBc of a -18 dBFS tone). Every other reference tone in eco
stays under 1e-4 FS (8 kHz: 7.8e-5).

**Other costs taken out.** Drive and output gains are computed once per
block when their smoothers are not moving (the same bits as per sample).
The quick-mode denormal check runs at eco (the golden settings are
standard): the PR suite went from 96.6 s to 33 s on this host.

**Figures**, i7-11800H, Docker Desktop on Windows 11, sonora-build:trixie,
GCC 14 Release, host loaded by other sessions; 60 s renders, per-block
minimum of 5 runs; `--cpu-calibrate`: reference kernel 83.88 / 83.80 us
before / after (drift 0.09 %), runner scale 2.572.

| Setting | p99.9 before | p99.9 after | after, reference-equivalent | max after (ref-eq.) |
|---|---|---|---|---|
| eco, golden settings, programme | 2032 us | 113.5 us | 44.1 us | 55.4 us |
| eco, +12 dB programme, drive 24 dB, motion on | 7052 us | 139.4 us | 54.2 us | 62.7 us |
| standard (4x), programme | 2224 us | 298 us | 116 us | 123 us |
| standard, +12 dB, drive 24 dB, motion on | 11727 us | 618 us | 240 us | 288 us |
| high (8x), programme | 2565 us | 455 us | 177 us | 199 us |
| high, +12 dB, drive 24 dB, motion on | 20974 us | 1137 us | 442 us | 602 us |

"Before" is the RK4 build on the same host the same day, 20 s renders,
uncalibrated (the kernel read 83.4-83.9 us each time it was timed that
day, so the same scale applies). Eco holds class B (60 / 150 us) on both loads, calibrated:
`cpu.held` is `["eco"]`. The hot case has about 10 % to spare. Standard
and high are over it and reported: their 4x and 8x oversamplers alone
take 39 and 60 us of a block (the chain with the hysteresis replaced by a
gain, measured the same way), as ADR-0011 3.5 allows beside an eco that
meets it.

**Goldens.** Regenerated: the integrator changed. Against the RK4
goldens, the golden render differs by at most 2.7e-4 (44.1 kHz), 2.1e-4
(48 kHz), 1.0e-4 (96 kHz) FS. On the reference tones, standard's error
against the 512-substep reference went from 2.9e-5 (8 kHz) and 3.9e-5 (15
kHz) to 3.6e-7 and 1.0e-6 FS: the new figures are the closer ones. THD
(0.6387 %), the response, latency (exact), motion, non-finite, denormals
(no subnormal output), zipper (drive -70.6 dBFS) and hot-input figures are
unchanged to the digits reported. Aliasing to 8 kHz moved by at most
1.4 dB, all inside the bars: eco 44.1 kHz -42.9 to -42.8 dBc (bar -40),
standard 44.1 kHz -84.1 to -83.6 (bar -80), high 44.1 kHz -98.8 to -97.4
(bar -95).

## THD bars (2026-11-02)

`thd.nominal_pct` was a 5 % ceiling over a measured 0.64 %: a ceiling that
loose holds nothing, and a ceiling of any height passes a tape that stopped
saturating. THD+N of a 1 kHz tone at -18 dBFS is now held as a band, at
drive 0 dB and at drive 12 dB, in every quality, 48 kHz. Measured (GCC 14
Release, Docker Desktop, i7-11800H, `SONORA_DSP_QUICK=1`), %:

| fs | drive | eco | standard | high |
|---|---|---|---|---|
| 44.1 kHz | 0 dB | 0.6268 | 0.6381 | 0.6415 |
| 44.1 kHz | 12 dB | 2.2726 | 2.3100 | 2.3210 |
| 48 kHz | 0 dB | 0.6288 | 0.6387 | 0.6416 |
| 48 kHz | 12 dB | 2.2794 | 2.3121 | 2.3216 |
| 96 kHz | 0 dB | 0.6387 | 0.6416 | 0.6424 |
| 96 kHz | 12 dB | 2.3121 | 2.3216 | 2.3241 |

The spread follows the internal rate, not the quality (eco at 96 kHz and
standard at 48 kHz both run the curve at 192 kHz and agree to the digit),
and stays under 2 % of the figure.
The bands are standard at 48 kHz +-5 %: drive 0 dB 0.607-0.671 %
(`thd.nominal_min_pct`, `thd.nominal_pct`), drive 12 dB 2.20-2.43 %
(`thd.drive12_min_pct`, `thd.drive12_pct`). Checked against a mutant that
drives the curve 1 dB harder and takes 1 dB off the output, so the
small-signal gain is unchanged and the unity and response bars do not see
it (the reference chain and the goldens do): eco, standard, high read
0.6926, 0.7035, 0.7067 % at drive 0 dB and 2.6186, 2.6558, 2.6666 % at
drive 12 dB, outside both bands in every quality. A +-10 % band (0.57-0.71 %,
tried first) let all three through at drive 0 dB. The second harmonic sits
at -146 dBc (the loop is symmetric), and `thd.h2_below_h3_db` holds it 90 dB
under the third (measured -102.7 dB; the bar was -20 dB before the
native.analog tape task tightened it).

### Regenerating the THD rows

The rows are written by the harness, not typed in. Run from the repository
root, after the build has produced `sonora-dsp-measure`:

    mkdir -p build/dsp-reports && SONORA_DSP_QUICK=1 sonora-dsp-measure --processor tape --quality plugins/native/tape/quality.json --golden plugins/native/tape/tests/golden --report build/dsp-reports

`sonora-dsp-measure` is the `sonora-dsp-measure` target of the build tree
(`plugins/native/measure/app`, linked at the tree's top level); put its
directory on `PATH` first. Run once on 2026-11-09, Release, `sonora-build:trixie`,
`SONORA_DSP_QUICK=1` set for the run: exit 0, "tape: meets its bars (0 failing)",
and `build/dsp-reports/tape.json` written. Its -18 dBFS standard rows read
drive 0 dB 0.6387 % and drive 12 dB 2.3121 %, the 48 kHz standard figures in the
table above.

The measured rows land in `build/dsp-reports/tape.json` under
`data.drive_level_gain_thdn_h2_h3`: one row per drive (0, 12 dB) and level
(-36, -18, 0 dBFS in quick mode; -48 to +6 dBFS in 6 dB steps in the full
run), each `[drive, level, gain, thd_pct, h2_dbc,
h3_dbc]`. The `-18 dBFS` rows are the ones the bands read. The bands
themselves are not written by the command: they are the `thd` keys in
`plugins/native/tape/quality.json`, set by hand from the standard-quality
figure at 48 kHz, +-5 %.

Scope of the bands, as the tape task left them: held at 48 kHz in all three
qualities, and in standard also at 44.1 and 96 kHz (`distortion()` in
`plugins/native/tape/tests/suite.cpp`, #286 review). The table above lists
every rate, but only those three checks are held.

## Open

Aliasing above 8 kHz;
goldens under Clang and MSVC (the substep count is discontinuous); the CPU
figures on a quiet reference machine (these are calibrated on a loaded one);
rt-safety-auditor run.
