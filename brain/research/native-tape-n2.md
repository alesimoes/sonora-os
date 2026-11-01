# Tape (ADR-0011 N2 slice 4): sources, measured bars, open defects

`org.sonora.tape`, DSP in `common/dsp/include/sonora/dsp/tape.h`, processor in
`plugins/native/tape`. Status: EXPERIMENTAL (bundles built and tested, not
installed unless `-DSONORA_INSTALL_EXPERIMENTAL=ON`) because it does not meet
its CPU class.

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
| reference chain (float vs double, hundreds of substeps) | 1e-4 FS | worst 6.8e-5 (eco, 8 kHz) |
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

## CPU (p99.9 per 128-frame block, 48 kHz stereo, 20 s, code path)

| Setting | p50 | p99.9 |
|---|---|---|
| eco | 892 us | 1990 us |
| standard | 723 us | 1213 us |
| high | 597 us | 1051 us |
| eco, +12 dB programme, drive 24 dB, motion on | 6062 us | 6469 us |

Class B asks 60 us. The cost is the RK4 step (up to 32 substeps, a tanh
each) per oversampled sample.

## Open

CPU class B (tabulated Langevin or a cheaper integrator, then hold
`cpu.held` and drop EXPERIMENTAL); aliasing above 8 kHz; `thd.nominal_pct`
is loose and should be re-measured; goldens under Clang and MSVC (the substep
count is discontinuous); rt-safety-auditor run.
