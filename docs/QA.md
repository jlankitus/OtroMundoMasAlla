# QA strategy

Three tiers, each catching what the tier below structurally cannot.
A visual system that can only be checked by looking at it is a visual
system nobody checks, and it rots quietly.

| Tier | Command | Catches |
|---|---|---|
| Unit tests | `ctest --preset windows` | math, physics, clock, canvas/camera in isolation |
| Single-frame QA | `python tools/qa_render.py <binary>` | invalid bytes the terminal receives |
| Temporal QA | `python tools/temporal_qa.py <binary>` | flicker, stale pixels, nondeterminism — properties of a *sequence* |

## Unit tests

Catch2, fetched by CMake. Physics is validated against independent
implementations of the same math (JPL's published table, closed-form
two-body results) rather than against itself. Conservation laws — energy
and angular momentum on a Kepler orbit — double as integrator regression
tests, because a leaking integrator fails them without any expected-value
table.

## Single-frame QA — `tools/qa_render.py`

Drives the real binary via `--snapshot` across sixteen option combinations
(every scenario, every colour depth, both block styles, odd sizes) and
asserts, per frame:

- decodes as UTF-8;
- exactly the requested cell geometry (this check found stdout in Windows
  text mode rewriting `\n` → `\r\n`: 121 cells where 120 were requested);
- contains only escape sequences we deliberately emit;
- SGR state is closed at frame end;
- every half-block glyph has an explicit foreground since the last reset
  (this check found the grey-artifact bug — and was itself verified to
  FAIL on the old binary before being trusted);
- stays inside a size budget (quantised shading exists because a
  continuous gradient tripled it);
- the scene is not empty.

With `--png-dir` it parses frames back into PNGs via `tools/ansi_to_png.py`
so they can be reviewed without a terminal. Text cells render as solid
blocks, not glyphs — presence and colour are checkable, HUD *legibility* is
not, and that limitation is why a hard-to-read HUD once passed QA.

## Temporal QA — `tools/temporal_qa.py`

`--snapshot` bypasses the game loop entirely, so flicker, stale pixels,
tearing and pacing lived in code no test executed. `omma-ascii --record N`
fixes that: it runs the *live* loop headlessly, substituting the only two
nondeterministic inputs — the wall clock becomes a fixed synthetic delta
(`--frame-delta`), the keyboard becomes one scripted character per frame
(`--keys`, `.` = none, `@` = prograde burn) — and writes every frame's
byte stream to disk. Software-in-the-loop, applied to a renderer.

`tools/vterm.py` replays those frames through a virtual terminal that keeps
its cell buffer **across** frames. That persistence is the point: `[H` plus
overwrite means frame N inherits whatever frame N−1 left in any cell it
does not touch, and a stateless parser cannot see a stale pixel by
construction.

The assertions:

- **paused frames are byte-identical** — nothing moves and the reported fps
  is pinned, so *any* difference is nondeterminism in the render path;
- **every cell is written every frame** — an unwritten cell is a stale
  pixel waiting to happen;
- **a re-run is byte-identical** — determinism of the whole recording path.

Motion is projected into still images, reviewable in a log:

- **space-time slice** — one screen row from every frame, stacked, so the
  vertical axis is time. A stationary body is a vertical line, a moving
  planet a diagonal, flicker a dashed line, tearing a horizontal break.
- **change heatmap** — how many times each cell changed; paused, it must be
  black.
- **filmstrip** — frames in a grid, for ordinary looks-right checking.

`vterm.py` understands only the escapes omma-ascii emits and asserts loudly
on anything else. A permissive parser here would hide the very bugs it
exists to find.
