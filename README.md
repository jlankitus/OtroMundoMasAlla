# OtroMundoMasAlla

A deterministic solar system and spacecraft simulator, written in C++20.

Real SI units. Real gravity. Planets propagated analytically, spacecraft
integrated numerically, both behind one interface. Fixed timestep, seeded RNG,
bit-reproducible runs. It renders in your terminal.

> **Status:** `v0.5.0` — opens with satellites already in orbit. Launch more,
> burn, and watch the predicted orbit deform.

```
                                                                                      ....
 Sun                                                                                      ...
   the frame origin                ..........................                                ..
                           ........                          .........                         .
                                     ..................               .....
                              ........                 ........            ....
                           ....                                .....           ...
                                                                   ....           ...
                                      ................                 ...          ..
                                  .....              .....               ...         ...
                                ...                      ...               ..          .
                               ..             .%%%.         *** Mercury     ..          .
        .          ..          .              .%%%.Sun      ***              .          .
        .           ..         ..              ...           .               .         ..
         .           ..         ..                          ..              ..         .
         ..           ...        ...                      ...              ..        ..
           ..           ...         .....              ....             ...         ..
             ...           .....        ...............              ....        ...
               ....            ....%%% Venus                    ......        ....
                  .....            %%%...........................         ....
                       .....                                   ###  ......                   ...
.                           .........                       ...+++...                     ....
 ....                                .......................   +++                     ...
```

*2:1 dimetric, ASCII density mode. In a truecolour terminal these are shaded
discs with a corona on the Sun.*

---

## Why this exists

Two goals, and they turn out to be the same goal:

1. **Build something beautiful.** Launch satellites, fly constellations, watch
   orbital mechanics do the counter-intuitive things orbital mechanics does.
2. **Build it the way flight dynamics software is actually built.** Deterministic
   core, headless-first, validated against independent implementations of the
   same physics, Monte Carlo capable, tested in CI.

The second constraint is what makes the first one achievable. A simulator you
cannot reproduce is a simulator you cannot debug, and a simulator you cannot
debug stops being fun at roughly the same moment it stops being correct.

## Architecture

```
core  ──▶  physics  ──▶  sim  ──▶  apps
```

Dependencies point one way and the build system enforces it. `core` knows
nothing about orbits. `physics` knows nothing about scheduling. `sim` knows
nothing about rendering. `apps` are interchangeable clients.

| Layer | Contains | Knows nothing about |
|---|---|---|
| `src/core` | units, `Vec3`, `Epoch`, fixed-step clock | orbits |
| `src/physics` | `IEphemeris`, Kepler, `GravityField`, integrators | time warp, scheduling |
| `src/sim` | world, scheduler, spacecraft, telemetry | pixels |
| `apps/omma-ascii` | game loop, key bindings, which glyph a planet gets | physics internals |

A Unity or Unreal front-end, if it ever happens, is one more entry in `apps/`.
The simulator is the product; the renderer is a client.

### The central design decision

Celestial bodies and spacecraft are propagated differently, because they have
genuinely different capabilities in time:

|  | Kepler body | Integrated body |
|---|---|---|
| "Where are you at `t = +5 years`?" | one function call | must step there |
| "Where were you 3 days ago?" | one function call | gone, unless stored |
| Access pattern | **random access in time** | **sequential only** |

So they get different interfaces. `IEphemeris::sample(t)` is the *environment* —
valid at any instant, including the sub-step times an RK4 stage asks for.
Spacecraft are integrated against that environment.

This is not a simplification. It is what SPICE, GMAT, STK and Orekit all do:
planetary positions are looked up, not simulated. Full n-body remains available
as an `IEphemeris` implementation, where it earns its keep as a **cross-check** —
two independent implementations of the same physics that must agree.

It is also what makes unlimited time warp possible. A coasting spacecraft can be
promoted onto its own Kepler ellipse ("on rails"), at which point its position is
a function call and warp is unbounded.

## Building

Requires CMake ≥ 3.21 and a C++20 compiler. Catch2 is fetched automatically.

**Windows (MSVC):**

```bash
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

**Linux:**

```bash
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

**Strict, the way CI builds it:**

```bash
cmake --preset ci && cmake --build --preset ci && ctest --preset ci
```

## Running it

```bash
./build/windows/bin/Release/omma-ascii
```

Opens on the `leo` scenario: three satellites already in orbit on visibly
different inclinations, camera framed on Earth. Nothing to press to see the point.

```bash
./build/windows/bin/Release/omma-ascii --scenario constellation
./build/windows/bin/Release/omma-ascii --scenario transfer
./build/windows/bin/Release/omma-ascii --scenario empty      # just the planets
```

| scenario | |
|---|---|
| `leo` *(default)* | ISS inclination, sun-synchronous, and a low-inclination comms orbit |
| `constellation` | twelve satellites, four near-polar planes, Iridium-style Walker layout |
| `transfer` | one craft with a 900 m/s prograde burn already commanded — apoapsis climbs from 400 km to 4 800 km while periapsis stays put |
| `empty` | no spacecraft |

| key | |
|---|---|
| `space` | pause / resume |
| `-` `=` | time warp down / up, from real time to 20 years per second |
| `[` `]` | zoom out / in |
| `1`-`6` | zoom presets: low orbit, Earth-Moon, inner system, to Jupiter, to Neptune, everything |
| `L` | launch a satellite around the body in view |
| `.` `,` | burn prograde / retrograde, 10 m/s a press |
| `'` `;` | burn normal / anti-normal (changes inclination) |
| `k` | cut the engine |
| `r` `t` | tilt the board down / up |
| `z` `x` | spin the board left / right |
| `v` | snap between top-down and 2:1 dimetric |
| `w a s d` | pan |
| `f` | frame-timing breakdown |
| `tab` / `p` | next / previous body to follow |
| `c` | re-centre on the followed body |
| `o` `l` | orbit trails, labels |
| `?` | help |
| `q` | quit |

`omma-ascii --snapshot` renders a single frame to stdout and exits: plain text,
diffable, and something CI can assert against. A visual system that can only be
checked by looking at it is a visual system nobody checks.

```bash
./build/windows/bin/Release/omma-ascii --snapshot --zoom 2 --focus Earth --date 2030-03-21
```

`--colour truecolour|ansi16|ascii` picks the output depth. The `ascii` mode maps
pixel brightness onto a density ramp, which is what keeps snapshots diffable —
a pixel renderer that produces a blank rectangle without colour is a renderer
that has stopped being tested.

`--tilt DEG` and `--spin DEG` set the camera; `90` is top-down, `30` is the
2:1 dimetric projection RollerCoaster Tycoon uses. `--blocks full` drops to one
pixel per cell for terminals that cannot decode UTF-8.

## Render QA

Unit tests check `Canvas` and `Camera` in isolation. They cannot check the thing
that actually breaks: whether the *bytes the terminal receives* are valid and
self-consistent once a whole scene has been composed.

```bash
python tools/qa_render.py ./build/windows/bin/Release/omma-ascii --png-dir /tmp/shots
```

### Temporal QA

Everything above checks a *single* frame. Flicker, stale pixels, tearing and frame
pacing are properties of a **sequence** — and `--snapshot` bypasses the game loop
entirely, so the code where those bugs live had never been executed by a test.

```bash
python tools/temporal_qa.py ./build/windows/bin/Release/omma-ascii --frames 90
```

`omma-ascii --record N` runs the *live* loop headlessly, substituting the only two
nondeterministic inputs — the wall clock becomes a fixed synthetic delta, the
keyboard becomes one scripted character per frame — and writes every frame's byte
stream to disk. Which is software-in-the-loop applied to a renderer.

`tools/vterm.py` then replays those frames through a virtual terminal that keeps
its cell buffer **across** frames. That persistence is the point: `[H` plus
overwrite means frame N inherits whatever frame N−1 left behind, and a stateless
parser cannot see a stale pixel by construction.

Two assertions do the real work:

| | |
|---|---|
| **paused frames are byte-identical** | nothing moves and the reported frame rate is pinned, so *any* difference is nondeterminism in the render path |
| **every cell is written every frame** | a cell the frame didn't write is a stale pixel waiting to happen |

And motion is projected into still images, because you cannot review a video in a
text log:

- **space-time slice** — one screen row from every frame, stacked, so the vertical
  axis is *time*. A stationary body is a vertical line; a moving planet a diagonal;
  **flicker a dashed line**; tearing a horizontal break. Same trick as a slit-scan
  photograph.
- **change heatmap** — how many times each cell changed. Paused, it must be black.
- **filmstrip** — frames in a grid, for ordinary looks-right checking.

### Single-frame QA

Drives the real binary across thirteen option combinations and asserts the
output decodes as UTF-8, has exactly the requested cell geometry, contains only
escapes we deliberately emit, closes its SGR state, and stays inside a size
budget. With `--png-dir` it also parses each frame back into a PNG via
`tools/ansi_to_png.py`, so the frames can be looked at without a terminal — which
is how three of the four bugs in this pass were found.

## A note on floating point

The build never enables `-ffast-math` or `/fp:fast`. Those permit the compiler
to reassociate floating-point expressions, so `(a + b) + c` may silently become
`a + (b + c)` — a different answer under IEEE-754. That is how you end up with a
simulator that produces one trajectory in Debug and another in Release. We pay
the performance for reproducibility, and we pay it deliberately.

## Roadmap

- [x] **0.1** project skeleton, CMake, test harness
- [x] core: `Vec3`, typed units, `Epoch`, deterministic fixed-step clock
- [x] physics: `IEphemeris`, Kepler propagation, real solar system data
- [x] render: half-block truecolour canvas, dimetric camera, time warp, HUD
- [x] physics: gravity field, four integrators, energy-conservation tests
- [x] sim: world, spacecraft, launch, burns, propellant, collisions
- [x] render: spacecraft, predicted paths, shaded bodies
- [ ] **0.4** CI on Windows and Linux
- [ ] **0.5** perturbations: J2, third-body, drag
- [ ] **0.6** constellations, ground tracks, coverage
- [ ] **0.7** radio links and link budgets
- [ ] **0.8** swarm behaviour, intercepts, engagements
- [ ] **1.0** Monte Carlo runner, record/replay, 6DOF attitude

## Licence

MIT. See [LICENSE](LICENSE).
