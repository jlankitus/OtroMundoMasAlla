# OtroMundoMasAlla

A deterministic solar system and spacecraft simulator, written in C++20.

Real SI units. Real gravity. Planets propagated analytically, spacecraft
integrated numerically, both behind one interface. Fixed timestep,
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
core  ──▶  physics  ──▶  render / sim  ──▶  apps
```

Dependencies point one way and the build system enforces it.

| Layer | Contains | Knows nothing about |
|---|---|---|
| `src/core` | units, `Vec3`, `Epoch`, fixed-step clock, console facade | orbits |
| `src/physics` | `IEphemeris`, Kepler, `GravityField`, integrators | scheduling, spacecraft |
| `src/sim` | `World`, spacecraft, launches, burns, events | pixels |
| `src/render` | half-block canvas, dimetric camera | what it is drawing |
| `src/ffi` | `omma.dll` — the C ABI game engines load | anything C++ across the boundary |
| `apps/omma-ascii` | game loop, key bindings, HUD | physics internals |
| `apps/omma-unity` | Unity client: P/Invoke wrapper + 3D view ([setup](apps/omma-unity/README.md)) | sim internals |

A Unity or Unreal front-end, if it ever happens, is one more entry in `apps/`.
The simulator is the product; the renderer is a client.

The design arguments — why planets are looked up while spacecraft are
integrated, where the determinism boundary sits, why the gravity field is a
flat POD array — live in [docs/DESIGN.md](docs/DESIGN.md). The vocabulary is
in [docs/GLOSSARY.md](docs/GLOSSARY.md).

## Building

Requires CMake ≥ 3.21 and a C++20 compiler. Catch2 is fetched automatically.

```bash
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

`--preset linux` on Linux; `--preset ci` builds with warnings as errors, the
way CI does.

## Running it

```bash
./build/windows/bin/Release/omma-ascii
```

Opens on the `leo` scenario: three satellites already in orbit on visibly
different inclinations, camera framed on Earth. Nothing to press to see the
point.

| `--scenario` | |
|---|---|
| `leo` *(default)* | ISS inclination, sun-synchronous, and a low-inclination comms orbit |
| `constellation` | twelve satellites, four near-polar planes, Iridium-style Walker layout |
| `transfer` | one craft with a 900 m/s prograde burn already commanded — apoapsis climbs from 400 km to 4 800 km while periapsis stays put |
| `empty` | no spacecraft |

| key | |
|---|---|
| `space` | pause / resume |
| `-` `=` | time warp down / up, from real time to 20 years per second |
| `[` `]` `1`-`6` | zoom, and presets from low orbit out to the whole system |
| `L` | launch a satellite around the body in view |
| `.` `,` | burn prograde / retrograde, 10 m/s a press |
| `'` `;` | burn normal / anti-normal (changes inclination) |
| `k` | cut the engine |
| `g` | ground-track map: trail, coverage footprints, lat/lon |
| `r` `t` `z` `x` | tilt and spin the board; `v` snaps top-down ↔ dimetric |
| `w a s d` `c` | pan, re-centre |
| `tab` `p` | next / previous focus target |
| `o` `l` `f` | orbit trails, labels, frame timings |
| `?` | help |
| `q` | quit |

`--snapshot` renders one frame to stdout and exits — plain text, diffable,
assertable. `--record N` runs the live loop headlessly with scripted inputs
and writes every frame's bytes to disk. `--help` lists the rest.

## Testing

```bash
ctest --preset windows                                            # 130 unit tests
python tools/qa_render.py   ./build/windows/bin/Release/omma-ascii  # frame validity
python tools/temporal_qa.py ./build/windows/bin/Release/omma-ascii  # motion, flicker, determinism
```

Three tiers: unit tests for the math, a single-frame harness for the bytes the
terminal receives, and a temporal harness that records the live loop and reads
motion out of still images. What each tier catches, and why a stateless
frame-checker structurally cannot find a stale pixel, is in
[docs/QA.md](docs/QA.md).

## A note on floating point

The build never enables `-ffast-math` or `/fp:fast`: reassociation gives you a
simulator that produces one trajectory in Debug and another in Release. We pay
the performance for reproducibility, deliberately.

## Roadmap

- [x] core: `Vec3`, typed units, `Epoch`, deterministic fixed-step clock
- [x] physics: `IEphemeris`, Kepler propagation, real solar system data
- [x] physics: gravity field, four integrators, energy-conservation tests
- [x] render: half-block truecolour canvas, dimetric camera, HUD
- [x] sim: world, spacecraft, launch, burns, propellant, collisions
- [x] scenarios: leo, constellation, transfer — satellites on screen at launch
- [x] CI on Windows and Linux
- [x] C ABI (`omma.dll`) + Unity client — see [docs/ENGINES.md](docs/ENGINES.md)
- [x] perturbations: J2, third-body, and atmospheric drag — all validated against closed-form rates
- [x] ground tracks, horizon footprints, and the `g`-key coverage map
- [ ] radio links and link budgets
- [ ] swarm behaviour, intercepts, engagements
- [ ] Monte Carlo runner, record/replay, 6DOF attitude

## Licence

MIT. See [LICENSE](LICENSE).
