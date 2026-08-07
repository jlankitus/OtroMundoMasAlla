# OtroMundoMasAlla

A deterministic solar system and spacecraft simulator, written in C++20.

Real SI units. Real gravity. Planets propagated analytically, spacecraft
integrated numerically, both behind one interface. Fixed timestep, seeded RNG,
bit-reproducible runs. It renders in your terminal.

> **Status:** `v0.2.0` — eleven bodies orbiting on a tilted board, drawn with
> truecolour half-block pixels. No spacecraft yet.

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
| `src/physics` | `IEphemeris`, Kepler, gravity, integrators | time warp, scheduling |
| `src/sim` | world, scheduler, spacecraft, telemetry | pixels |
| `apps/omma-headless` | scenario runner, CSV telemetry, exit codes | — |
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

| key | |
|---|---|
| `space` | pause / resume |
| `-` `=` | time warp down / up, from real time to 20 years per second |
| `[` `]` | zoom out / in |
| `1`-`5` | zoom presets: Earth-Moon, inner system, to Jupiter, to Neptune, everything |
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
2:1 dimetric projection RollerCoaster Tycoon uses.

The headless driver prints the solar system as a table, plus a live
demonstration of the clock, time warp and determinism machinery:

```bash
./build/windows/bin/Release/omma-headless
```

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
- [ ] **0.2** gravity field, RK4 and Verlet integrators, energy-conservation tests
- [ ] **0.3** world, scheduler, spacecraft, launch and burn — *launch something*
- [ ] **0.4** CI on Windows and Linux
- [ ] **0.5** perturbations: J2, third-body, drag
- [ ] **0.6** constellations, ground tracks, coverage
- [ ] **0.7** radio links and link budgets
- [ ] **0.8** swarm behaviour, intercepts, engagements
- [ ] **1.0** Monte Carlo runner, record/replay, 6DOF attitude

## Licence

MIT. See [LICENSE](LICENSE).
