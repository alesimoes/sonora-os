# Qt shell render cost: 18 meters at 60 Hz, CPU rasteriser only

Measured 2026-10-12 with `tools/build-ui.sh --measure 20`, branch
`feature/qt-shell`. **Every figure here is llvmpipe (Mesa's CPU rasteriser)
in a container on a 16-CPU Windows/Docker host.** It is the cost of drawing
on a CPU, which ADR-0006 already rules out as a supported configuration. It
says nothing about frame rate or cost on the target's GPU. That measurement
needs the appliance, with the engine's xrun and callback counters beside it.

## Method

`sonora-ui --demo --meter-hz 60 --measure 20`: 2 s of warm-up, then 20 s
measured from inside the process (`ui/qt/src/measure.cpp`). The baseline is the
same shell with `--no-meters`. The shell ran twice: as an xcb client on Xvfb
and as a Wayland client under `cage` (wlroots headless backend, pixman).

The Qt software scene graph (`QT_QUICK_BACKEND=software`, which Qt Quick
uses where no OpenGL context can be created) cannot draw a custom `QSGGeometryNode`. On
that path the meters draw nothing and the shell says so in red. So the only
software path that can be measured is OpenGL on llvmpipe.

## Results

| Run | fps | frame p50 / p99 | GUI tick p50 / p99 | process CPU | llvmpipe threads |
|---|---|---|---|---|---|
| Xvfb, 18 meters | 62.5 | 15.97 / 17.82 ms | 0.039 / 0.346 ms | 95.7 % of a core | 86.2 % |
| Xvfb, no meters | 15.6 | 64.0 / 66.3 ms | 0.004 / 0.340 ms | 23.1 % | 19.9 % |
| cage, 18 meters | 62.1 | 16.08 / 18.39 ms | 0.033 / 0.330 ms | 99.8 % | 85.2 % |
| cage, no meters | 15.6 | 64.0 / 66.8 ms | 0.005 / 0.394 ms | 25.3 % | 20.4 % |

The render thread's own time (beforeRendering to afterRendering, 0.15 to 0.20
ms p50) leaves out the rasterisation. llvmpipe does that on its own worker
threads, and their time appears only in the per-thread CPU column. The
no-meters baseline runs at 15.6 fps because only the clock text changes.

## What it means

- Pulling and feeding 18 meters costs the GUI thread about 0.04 ms per tick.
  The tick is not the problem.
- On a CPU rasteriser, 18 meters at 60 Hz cost about one full core, about 75
  % of a core above the baseline, nearly all of it in llvmpipe. That is the
  CPU the audio callback would lose on a machine that silently fell back.
  For that reason the shell drops itself to 30 Hz on a software rasteriser
  unless `--meter-hz` says otherwise, and puts the rendering path on screen
  in red (ADR-0006).
- 60 Hz held on both platforms: the frame interval p99 stayed under 18.4 ms.
