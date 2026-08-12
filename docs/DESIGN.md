# Design notes

The rationale behind the decisions that shaped this codebase. Code comments
state the contract and the local *why*; the long-form arguments live here so
they are told once, together, instead of scattered across file headers.

Companion documents: [GLOSSARY.md](GLOSSARY.md) for the orbital-mechanics
vocabulary, [QA.md](QA.md) for how the renderer is tested.

---

## 1. The layering

```
core  ──▶  physics  ──▶  render / sim  ──▶  apps
```

Dependencies point one way and CMake link targets enforce it — a `physics`
file that includes a `sim` header fails to *link*, not just to review.

- `core` — units, `Vec3`, `Epoch`, the fixed-step clock, the console facade.
  Knows nothing about orbits.
- `physics` — ephemerides, Kepler propagation, gravity, integrators.
  Knows nothing about scheduling or spacecraft.
- `sim` — `World`, `Spacecraft`, launches, burns, collisions, events.
  Knows nothing about pixels.
- `render` — canvas, camera. Knows nothing about what it is drawing.
- `apps` — interchangeable clients. A Unity front-end, if it ever happens,
  is one more entry here.

## 2. Ephemerides vs. integration — the central decision

Celestial bodies and spacecraft have genuinely different capabilities in
time, so they get different interfaces:

|  | Kepler body | Integrated body |
|---|---|---|
| "Where are you at t = +5 years?" | one function call | must step there |
| "Where were you 3 days ago?" | one function call | gone, unless stored |
| Access pattern | **random access in time** | **sequential only** |

`IEphemeris::sample(t)` is the *environment* — valid at any instant,
including the sub-step epochs an RK4 stage asks for (`t`, `t+h/2`, `t+h`).
Spacecraft are integrated against that environment. This mirrors SPICE,
GMAT, STK and Orekit: planetary positions are looked up, not simulated.

It is also what makes unlimited time warp affordable. With every body
analytic, advancing the clock a billion ticks is one integer addition; the
cost of warp is the cost of whatever is being *integrated*. A coasting
craft can later be promoted onto its own ellipse ("on rails",
`PropagationMode`), making its position a function call too.

## 3. Determinism

**The tick count is the time.** `SimClock::now()` is derived as
`start + fixedStep × ticks` — an exact integer computation — never
accumulated in floating point. Adding 0.1 s a million times in a `double`
drifts by milliseconds; the tick count never drifts because it cannot.

**Epochs are int64 nanoseconds since J2000.** Exact equality between epochs
is meaningful, which is what lets `GravityField::refresh()` memoise on the
epoch (§5) — a float epoch would miss the cache on the last bit and the
optimisation would silently do nothing.

**The nondeterminism boundary is explicit.** `StepPacer` reads the wall
clock and converts ragged frame times into a whole number of fixed steps
(carrying the remainder); `SimClock` is everything below that line. A
recorded sequence of step counts replays bit-identically anywhere. The
pacer also clamps runaway frames and *discards* the backlog: carrying it
would make the next frame ask for even more steps — the spiral of death.
Simulated time falling behind the wall clock is correct behaviour;
pretending otherwise is not.

**No `-ffast-math`, ever.** It licenses the compiler to reassociate
floating-point expressions, so `(a+b)+c` may become `a+(b+c)` — a different
answer under IEEE-754, and a simulator that produces one trajectory in
Debug and another in Release. `-ffp-contract=off` for the same reason.

## 4. Time warp

Warp changes *how many* fixed steps run per frame, never how big a step is.
`dt` is identical on every row of the warp ladder, which is what keeps runs
reproducible across warp settings. Discrete presets ("1 day/s") rather than
a continuous multiplier, because you almost always want a named timescale.

Once spacecraft are being integrated, the steps-per-frame cap is expressed
in **craft-steps divided by fleet size**: a fixed cap holds the frame rate
for one craft and collapses it for twelve (measured: 7 fps). The honest
consequence — simulated time falls behind sooner with more craft — is shown
on the HUD as `[falling behind]`.

## 5. Put polymorphism where N is small, data layout where N is large

`GravityField` is the worked example. The naive design calls
`IEphemeris::sample(t)` inside the per-spacecraft acceleration loop: eleven
bodies × four RK4 stages × ten thousand craft is 440,000 virtual calls per
step, each re-solving Kepler for a planet that has not moved. Instead:

- `refresh(t)` — eleven virtual calls, once per integrator stage, flattening
  every source into a contiguous array of `{position, gm}` PODs;
- `accelerationAt()` — a tight loop over those PODs, per craft, with no
  virtual dispatch.

The interface sits at the eleven-planets boundary, not the
ten-thousand-satellites boundary. Most "OOP is slow" pain comes from
getting that the wrong way round.

`refresh()` is additionally **memoised on the epoch**, because every craft
in a step integrates over the *same* four stage epochs. Before the memo:
4N refreshes per step instead of 4, measured at 76 ms/frame with three
satellites (13 fps). After: 1–6 ms/frame.

The two-phase refresh-then-query API invites bugs (forget the refresh and
you integrate against last stage's planets). It survives because the only
caller is `integrate()`, which owns both halves and is itself tested. When
an API is easy to misuse, confine the use to one place and test that place
hard.

`Spacecraft` is a flat POD struct for the same reason: a fleet of thousands
is a data-layout (SoA) change, not a rewrite.

## 6. The World tick

Everything `World` does happens inside `step()`, in a fixed order that is
part of the contract because changing it changes the answers:

1. advance the clock
2. refresh gravity (implicitly, per integrator stage)
3. integrate every spacecraft, applying thrust
4. burn propellant, expire finished burns
5. update each craft's central body and elements
6. detect collisions

Collisions before integration would let a craft pass through a planet
during the step it hit it; propellant before integration would charge for
thrust not yet applied.

Two sim details that were bugs before they were rules:

- **Burn cutoff is honoured within a step** via a duty cycle. A 0.19 s burn
  inside a 1 s step must deliver 19% of a step's impulse, not 100% —
  measured as 5.2× the commanded delta-v before the fix.
- **Mass uses the midpoint rule** within a step. Holding mass at
  start-of-step delivers `Δm/m₀` instead of `ve·ln(m₀/m₁)` — 2.7% low on a
  long burn.

Events (`SimEvent`) are a drained queue, not callbacks: a callback fires
mid-tick when the world is between consistent states, and a listener that
mutates the world from inside `step()` is a class of bug that is very hard
to reason about. A queue is also recordable, so a replay can assert the
same things happened.

## 7. Rendering choices

- **Half-block pixels.** Each character cell holds `▀` with independent
  foreground and background: two square pixels per cell, which makes the
  camera's unit aspect 1.0 instead of the 2.0 a character grid needs. Pass
  2.0 anyway and every orbit comes out an egg.
- **2:1 dimetric, not true isometric** — the RollerCoaster Tycoon
  projection, 30° elevation. It is what makes inclination visible at all.
- **Orbits are polylines sampled in eccentric anomaly.** Uniform *time*
  clusters samples at apoapsis and leaves the fast periapsis arc full of
  gaps; uniform eccentric anomaly spreads points evenly around the figure.
  Segments (clipped by the canvas, Cohen–Sutherland) rather than scattered
  points, so 256 samples suffice at any zoom.
- **Painter's algorithm by camera depth**, not by GM — sorting by mass puts
  the Sun on top even when Mercury passes in front of it.
- **The focused orbit is the one bright line.** Eleven dimmed orbits at
  similar brightness render as four indistinguishable browns; a star-chart
  scheme (one quiet slate for reference orbits, the subject in its own
  colour) is what reads.
- **Following a craft centres on its central body**, not on the craft.
  Centring the craft swings the whole world around it every 90 minutes and
  pushes the far half of its own orbit off screen — exactly the part you
  are watching while burning. The orbit is the subject, not the vehicle.
- **Shading is quantised to 24 levels.** A continuous gradient gives every
  pixel its own SGR escape: 48 KB/frame, versus ~14 KB quantised, with no
  visible banding.

## 8. Scenarios

A named, fully deterministic starting state — fixed altitudes, fixed
phasing, no randomness — so a scenario name plus a frame count is a
reproducible test case the QA harness can assert on.

The default is **not** an empty sky. The point of the simulator is
spacecraft; a first frame with none shows off the part that was already
finished. Each scenario also picks its own warp: at one day per second a
satellite laps its planet fourteen times a second, which is unwatchable
*and* pointlessly expensive to integrate.

## 9. Game Programming Patterns, applied

| Pattern | Where |
|---|---|
| Strategy | `IEphemeris` implementations |
| State | `PropagationMode` (Integrated ↔ OnRails) |
| Game Loop / Fix Your Timestep | `StepPacer` + `SimClock` |
| Event Queue | `SimEvent`, drained per frame |
| Data Locality | `GravityField` POD cache, flat `Spacecraft` |
| Facade | `core/console` — the only `#ifdef _WIN32` in the tree |

## 10. Lessons that cost something

Kept because each was paid for with a real bug.

- **Instrument before reasoning.** Every performance and rendering mystery
  in this project (22 fps, grey artifact blocks, 76 ms frames, "needs an
  interactive terminal") had a plausible first hypothesis that was wrong.
  Four timestamps, a byte-stream audit, or a printed failure reason settled
  in minutes what speculation could not.
- **Test the test.** The grey-block audit was only trusted after it was
  shown to FAIL on the old binary. The `vterm.py` reconstruction once came
  out blank because the *tool* was broken and the app was fine.
- **Wrong-looking constants deserve a comment.** The eccentricity-vector
  formula, the `GM_earth + GM_moon` sum, and the 4.5e7 m transfer framing
  (Earth sits at a *focus* of the ellipse, not its centre) all look like
  errors until you know why.
- **Near-circular orbits are the hard test case.** A transposed
  eccentricity-vector formula looks plausible on eccentric orbits and is
  badly wrong on the orbits real satellites fly.
- **Split hard invariants from sampling tolerances.** `minR >= rp` is a law;
  "sampled minimum within 1e-4 of periapsis" measures the sampling grid,
  and conflating them produces tests that fail for innocent reasons.
