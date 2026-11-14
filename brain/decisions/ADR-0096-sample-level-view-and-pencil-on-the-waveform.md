# ADR-0096 — The sample-level view and the Pencil on the waveform: read the WAV in the interface, write through `sample.edit draw`

Status: accepted (2026-11-08)

## Summary

ADR-0052 §4 delivered the engine's `sample.edit` (verbs repair, draw, silence,
reverse, invert, gain, normalize, fade_in, fade_out) and the `sample.*`
commands, and left out two things: a Pencil drawn with the pointer on the
waveform, and a view of individual samples. This ADR decides both with **no
protocol and no file-format change**. The interface gets a `SampleSource`
(`ui/qt/src/sample_view.h`, header-only, beside `PeakSource`) that answers
from memory a window of a take's WAV loaded on its own worker thread; the
sample view replaces the overview at 4 frames a pixel or closer; the Pencil on
a clip at that zoom becomes one `sample.edit draw`, one undo step, built by
the pure function `pencil_points`.

## Context

- Peak files bottom out at `kPeakBase` = 256 frames a bucket
  (`common/audiofile/include/sonora/peak_file.h`). Zoomed in past that, the
  overview interpolates between buckets: nothing true to draw on, and a Pencil
  drawn over an invented curve lies about what it replaces.
- The engine already owns the write: `sample.edit draw REGION FROM TO CHANNEL
  FRAME:LEVEL,...` (`engine/control/include/sonora/sample_edit.h`). FRAME is a
  **timeline** frame, points strictly increasing, levels -1..1, at most 4096
  points (`parse_sample_edit`), a stretch at most `kMaxSampleEditFrames`
  (16 million). The take is never written: the region's span is copied to a
  new file and the region points at it, one undo step.
- The interface already reads the session's files for overviews
  (`FilePeakSource`, its own worker thread, `peaks()` from memory only), so
  reading a WAV window there adds no new kind of access.
- The Edit window's deepest zoom is `kMinFpp` = 1 frame a pixel
  (`ui/qt/src/edit_controller_detail.h`).
- `ui/qt/CMakeLists.txt`, `ui/qt/tests/CMakeLists.txt`, `session_model.h` and
  `shell_model.cpp` are held by other active features.

## Decision

### 1. No protocol or format change

The Pencil ends in the existing `sample.edit draw` verb, sent through the
command sender that `sample.*` already uses. No new command, no new field in
`contracts/api`, no new file beside the takes.

### 2. `SampleSource`

`ui/qt/src/sample_view.h`, header-only, shaped like `PeakSource`:

- `read(source, channel, first, count, float* out)`: source frames, answered
  from memory only, zeros for a frame outside the source or not loaded yet.
  Called on the GUI thread.
- `ready(source)`: false while the window is still loading. While not ready
  the view says it is loading and the Pencil refuses: zeros are not the audio.
- `FakeSampleSource` (sine or ramp, channels phase-shifted) in the same header
  for the tests and the screenshot scenes.
- The real implementation (task `core`) loads a window of the WAV around what
  was asked on **its own worker thread**, never on the GUI thread and never on
  the audio thread; it calls back to redraw when the window lands, as
  `FilePeakSource` does.

### 3. When the sample view shows

- At or below `kSampleViewMaxFpp` = 4 frames a pixel the sample view replaces
  the overview: a line through the samples, per channel lane.
- At or below `kSampleDotsMaxFpp` = 0.25 frames a pixel (4 px a sample) each
  sample is also a dot. **Changed from the proposed 1 frame a pixel**: at one
  frame a pixel neighbouring dots touch and read as the line already drawn, so
  a dot carries information only once there is room between them. That needs
  zooming past `kMinFpp` = 1; the UI task lowers `kMinFpp` to 1/16 (16 px a
  sample). `frame_at` and `x_of` already work in doubles.

### 4. The Pencil

- Only on a clip at a zoom of `kSampleViewMaxFpp` or closer, on one channel
  lane (CHANNEL is that lane, from 1).
- It collects pointer positions (view x, level from y) and on release
  `pencil_points` turns them into Draw points: one point per pixel column
  crossed, the columns between two positions interpolated so a fast stroke
  skips none, levels clamped to -1..1, frames via the view's `scroll + x *
  fpp` (as `ViewState::frame_at`), in timeline frames as `sample_edit.h`
  documents. Where two columns land on one frame (past 1 frame a pixel) the
  later one wins; going back over a column redraws it.
- Columns off the clip draw nothing: a stroke stays inside one clip. FROM is
  the first point's frame, TO one past the last.
- The release sends **one** `sample.edit draw`: one undo step. More than 4096
  points or 16 million frames is refused before sending (the engine's limits,
  mirrored as constants since `ui/` does not depend on `engine/`; the contract
  test pins them). At 4 frames a pixel 4096 columns are 16384 frames, so the
  frame cap cannot bind; the point cap binds only for a stroke wider than
  4096 px.
- Levels are written with a decimal point by hand, not `printf`: Qt sets the C
  locale from the environment, and "0,5" would reach the engine's `strtod` as 0.
- The take is never written (ADR-0052 §4: the engine copies).

### 5. Refusals

A refusal of `sample.edit` (a take open, a stretch the verb refuses, a write
that failed) stays the held notice ADR-0052 §5 set, not the status line.

### 6. Coarser zoom

The Pencil on a waveform at a zoom coarser than `kSampleViewMaxFpp` does
nothing and says why on the status line ("zoom in to the samples to draw");
it is the operator's next action, not a failure to hold.

### 7. No new compiled or QML file in this feature

The build files and `session_model.h` / `shell_model.cpp` are held by other
features, so new code is header-only or goes in existing `.cpp` files, scene
files are globbed, the test extends `ui/qt/tests/test_clip_groups.cpp`, and the
panel lives in `EditWindow.qml`. A follow-up extracts `SampleEditPanel.qml`
and `sample_window.cpp` once those features merge.

## Rationale

- The engine's `draw` verb already is the destructive, undoable, take-safe
  write; a second write path would duplicate the copy, the undo and the
  refusal while a take is open.
- Reading in the interface mirrors `FilePeakSource`: the same access, the same
  thread discipline, nothing new on the wire. Samples over the protocol would
  be a large, new, versioned message for data the interface can read itself.
- A pure `pencil_points` is testable without a window, a pointer or an engine.
- 4 frames a pixel: a pixel then covers at most 4 samples, so the drawn line
  is the samples, and the Pencil's one point per column, joined by the
  engine's straight lines, is at most 4 samples of interpolation.

## Consequences

- One stroke, one undo step, one new file in "Audio Files/" (ADR-0052
  consequences: the session folder grows by the region's span per edit).
- A stroke is limited to 4096 columns; a wider one is refused with a reason.
- The interface process holds one WAV window per drawn source in memory; its
  size is the core task's to set against the measurement below.
- The view must show "loading" while `ready()` is false, and the Pencil
  refuses then.
- Follow-up after the held files merge: `SampleEditPanel.qml`,
  `sample_window.cpp`.

## Alternatives rejected

- **Samples over the protocol** (`sample.read`): a new message and protocol
  version for data on a disk the interface already reads; and the control
  thread would serve bulk reads.
- **Finer peak files** (a 1-frame level): a peak file as large as the WAV,
  and a format change.
- **One `sample.edit` per pointer move**: one undo step and one file copy per
  move; the operator expects one stroke, one undo.
- **Pencil on the overview**: draws over a curve that is not the audio.
- **Dots from 1 frame a pixel**: they touch and add nothing.
- **Formatting levels with `printf`**: locale-dependent decimal comma.

## Unverified

- How fast a WAV window of a 16M-frame clip loads from a USB stick: frames
  per second and memory held (handed to the researcher). It sets the window
  size and whether the view shows "loading" for long.
- Lowering `kMinFpp` to 1/16: the ruler ticks and grid at sub-frame zoom are
  not checked yet (UI task).
- The 4096-point engine cap's behaviour on a very wide display (over 4096 px):
  refused by design, not tried on hardware.
