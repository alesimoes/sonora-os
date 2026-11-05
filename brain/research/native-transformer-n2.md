# Transformer (Sonora Transformer), N2 slice 3 as an insert

The iron of a line or output transformer on its own, for tracking and mixing
analog sources. It reuses `common/dsp/transformer.h` (the tube drive's output
stage: a Jiles-Atherton core in a node equation, trapezoidal, one scalar
Newton solve per sample) with the supply stiff (`r_supply = 0`) and the rail
at 1e6 V, so the driver is a plain source and the core is the only
nonlinearity. Zero latency, no oversampler.

## Cores

All three share the loop shape of the output stage default (Ms/a = 4000,
k/Ms = 7.5e-5, c = 0.3, alpha = 2e-5) and differ in two numbers, both ours and
generic (no particular make): the low-field corner with Rs || RL = 566 ohm
across the winding, and the saturation flux. 1.0 FS = 100 V at the primary.

| core | corner | saturation flux | full scale saturates below |
|---|---|---|---|
| small | 30 Hz | 0.25 V s | about 64 Hz |
| line (default) | 12 Hz | 0.6 V s | about 27 Hz |
| large | 5 Hz | 1.2 V s | about 13 Hz |

Currents scale together and the inductance the other way, so the shape of the
loop is unchanged; `core_params()` in `src/transformer.cpp` derives them.

## Measured (48 kHz unless stated; Release, i7-11800H, Docker Desktop)

- Gain at the stated corner, -70 dBFS: -3.09 dB for all three cores (the
  Rayleigh-region slope and the trapezoid; the corner rises with level).
- Mid-band 1-20 kHz: within 0.005 dB of unity at every drive.
- Equal-flux law: (f/2, L - 6.02 dB) has 6.02 dB more THD+N than (f, L), within
  0.07 dB: the magnetising current is a function of flux alone.
- Line core, 0 dBFS: THD+N 1 kHz -66 dB, 100 Hz -49 dB, 40 Hz -38 dB. At the
  bass the relative distortion is not ordered by level (the loop opens, then
  saturates).
- Aliasing at nominal (drive 6 dB, -18 dBFS), worst over 1-19 kHz tones and
  44.1/48/96 kHz: small -82.3 dBc, line -97.2 dBc, large -110.7 dBc. ADR-0011
  3.2's -90 dBc is met by the default core; the small core departs (reported).
- CPU, q128 48 kHz stereo, 20 s render: p50 24.6 us, p99.9 29.9 us, max 31.2 us
  (class B bar 60 / 150 us), on a busy shared host.

## Not modelled

Leakage inductance and winding capacitance (the top end is flat), winding
resistance, the driver's own distortion (the tube drive has it), and hum
pickup. A SPICE reference for the core itself is the output stage's
(`native-tube-n2.md`); this processor's reference is the physics relations
above, not a netlist of a particular transformer.
