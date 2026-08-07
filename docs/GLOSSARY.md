# Glossary

Every term this codebase uses, in plain English. If a name in the source isn't
explained here, that's a bug in this file.

---

## Time and the loop

| Term | Meaning |
|---|---|
| **Epoch** | A specific instant in *simulated* time. The sim's "now". Unrelated to wall-clock time. We measure it in nanoseconds since **J2000** (2000-01-01 12:00:00 TT), the standard aerospace reference instant. |
| **Duration** | A span of simulated time. Also integer nanoseconds. |
| **dt / timestep** | How much simulated time one physics tick advances. Fixed. |
| **Tick / step** | One advance of the simulation by exactly `dt`. |
| **Time warp** | How many ticks we run per rendered frame. `1x` is real-time; `10000x` runs ten thousand ticks between two frames you see. |
| **Fixed timestep** | `dt` never varies with frame rate. A slow machine renders less often; it does not take bigger physics steps. |
| **Determinism** | Same inputs and same seed produce bit-identical outputs, on every run and every machine. |
| **Spiral of death** | The failure mode where a slow frame requires more catch-up steps, which makes the next frame slower, which requires more steps. Fixed by clamping steps-per-frame and accepting that sim time falls behind wall time. |

### Why determinism is non-negotiable

A bug that surfaces four hours into a run is unfixable if the run differs every
time. With determinism you replay it exactly, set a breakpoint, and walk into
it. Variable timesteps and wall-clock reads scattered through the physics are
the two things that destroy it, so this codebase confines both to a single
class at the edge (`StepPacer`) that the physics never sees.

### Why we never accumulate time in a float

`t += dt` a million times in a `double` accumulates rounding error, and the
error depends on the order and magnitude of the additions. Integer nanoseconds
are exact: a million additions of `1'000'000'000` ns is precisely a million
seconds, forever. We store time as `int64_t` nanoseconds and convert to
`double` seconds only at the moment the physics needs it.

Range: `int64_t` nanoseconds covers roughly ±292 years around J2000, i.e. 1708
to 2292. Ample, and the constraint is asserted rather than assumed.

---

## Where things are

| Term | Meaning |
|---|---|
| **State vector** | Position `(x, y, z)` plus velocity `(vx, vy, vz)`. Six numbers; the complete motion state of a point mass. |
| **Ephemeris** | Greek for "diary". A table or formula giving a body's position at any time. JPL's **DE440** is the authoritative one for our solar system. |
| **Orbital elements** | A different six numbers describing the same orbit — chosen so that five of them barely change. |
| **Propagate** | Advance a body's state forward in time. A *propagator* is the thing that does it. |
| **Barycenter** | The centre of mass a system orbits. The Earth–Moon barycenter sits ~1,700 km *below Earth's surface*, so Earth visibly wobbles around it. |
| **Reference frame** | The coordinate system positions are expressed in. Ours is heliocentric and inertial, aligned with the J2000 ecliptic. |

### The six orbital elements

| Element | Symbol | What it controls |
|---|---|---|
| Semi-major axis | `a` | Size of the ellipse |
| Eccentricity | `e` | Shape. `0` = circle, `0.7` = elongated, `≥1` = escape trajectory |
| Inclination | `i` | Tilt of the orbital plane. `0°` equatorial, `90°` polar |
| Longitude of ascending node | `Ω` (RAAN) | Rotation of the tilted plane about the reference axis |
| Argument of periapsis | `ω` | Rotation of the ellipse within its plane |
| True anomaly | `ν` | Where the body currently is along the ellipse — the only fast-changing one |

Position and velocity change every second; the first five elements are nearly
constant. That is the entire reason elements exist.

| Term | Meaning |
|---|---|
| **Apoapsis / periapsis** | Farthest / nearest point of an orbit. Around Earth: *apogee / perigee*. Around the Sun: *aphelion / perihelion*. |
| **Anomaly** | An angle measuring progress around an orbit. **True** (`ν`) is the real geometric angle; **eccentric** (`E`) is a mathematical helper; **mean** (`M`) advances at a perfectly constant rate. |

---

## Making things move

| Term | Meaning |
|---|---|
| **Kepler's equation** | `M = E − e·sin(E)`. No closed-form solution; solved numerically by Newton's method in ~4 iterations. This one equation is what makes analytic orbits possible. |
| **Numerical integration** | Approximating motion in small steps: "gravity pulls this hard here, so a second later you're roughly there." |
| **Euler** | The naive single-sample method. Consistently undershoots a curving path, so orbits spiral outward. Included only as a cautionary baseline. |
| **RK4** | Runge–Kutta 4th order. Samples acceleration **four times** per step and blends them. The workhorse; roughly 10,000× more accurate than Euler at the same `dt`. |
| **Symplectic** | A family (leapfrog, velocity Verlet) that is less accurate per step than RK4 but conserves **energy** indefinitely. Orbits stay closed over millions of years rather than slowly drifting. |
| **Energy drift** | The diagnostic. A closed orbit has constant total energy; an integrator that quietly adds energy makes the orbit spiral outward. Our primary correctness test. |
| **On rails** | Freezing a coasting spacecraft onto its own Kepler ellipse so its position becomes a function call. What makes unbounded time warp possible. |

### RK4 versus symplectic

Not a quality ordering, a trade. RK4 has smaller error over hours; symplectic
has bounded error over eons. We implement both and measure, because the answer
depends on what you're simulating.

---

## Changing an orbit

| Term | Meaning |
|---|---|
| **Delta-v (Δv)** | The velocity change a manoeuvre produces, in m/s. The universal currency of spaceflight — every mission is budgeted in it. Surface to low Earth orbit is ≈ 9,400 m/s; a satellite's entire lifetime of station-keeping might be 100 m/s. |
| **Burn** | Applying thrust. Changes velocity, which reshapes the ellipse. |
| **Hohmann transfer** | The cheapest two-burn path between two circular orbits. |
| **Perturbation** | Anything making a real orbit deviate from a perfect ellipse. |

### The perturbations that matter

- **J2** — Earth is not a sphere; it bulges at the equator. By far the largest
  perturbation for Earth satellites. It makes orbital planes slowly precess —
  and gets deliberately *exploited*: a **sun-synchronous** orbit is tuned so J2
  precession exactly matches Earth's motion around the Sun, keeping the
  satellite's lighting conditions constant. Nearly every earth-observation
  satellite uses this.
- **Third-body** — the Moon and Sun tugging on an Earth satellite.
- **Drag** — atmosphere. Matters below ~600 km; eventually deorbits you.
- **SRP** — solar radiation pressure. Photons pushing. Tiny, and never stops.

---

## Simulation engineering

| Term | Meaning |
|---|---|
| **Headless** | Runs with no window and no graphics. Required for CI and for batch runs. |
| **Telemetry** | The stream of measurements a simulation emits — position, power, temperature. The same word, and the same idea, real spacecraft use. |
| **Record / replay** | Log every input so a re-run reproduces the original bit for bit. |
| **Monte Carlo** | Run one scenario thousands of times with randomised dispersions — sensor noise, thruster misalignment, launch injection error — and read the statistics. "Does the mission succeed in 99.7% of cases?" This is how aerospace actually validates anything. |
| **Dispersion** | One randomised deviation from nominal in a Monte Carlo run. |
| **6DOF** | Six degrees of freedom: three position **plus three attitude**. 3DOF is position only. Real spacecraft need 6DOF because pointing *is* the job — antennas, solar panels, cameras. |
| **Closed-loop** | Sim produces sensor readings → flight software decides → commands actuators → sim responds. A genuine feedback loop, not scripted playback. |
| **SITL / HITL** | Software / Hardware In The Loop. Run the real flight *software*, or the real flight *computer board*, against a simulated spacecraft. HITL must run in true real-time and speak real serial and network protocols. |
| **Unit under test (UUT)** | Whatever the simulation is wrapped around — a control algorithm, a flight computer, a whole spacecraft. |

---

## Constellations and comms

| Term | Meaning |
|---|---|
| **Constellation** | A set of satellites arranged deliberately for coverage. GPS, Iridium, Starlink. |
| **Walker constellation** | The standard parameterisation of one: `i: T/P/F` — inclination, total satellites, orbital planes, phasing offset. |
| **Ground track** | The path traced on Earth's surface directly beneath a satellite. |
| **Footprint** | The region of the surface a satellite can currently see. |
| **Link budget** | Whether two nodes can actually close a radio link, given transmit power, distance, and antenna gain. What makes "radio network" a real mechanic instead of a line drawn between dots. |
| **Crosslink** | A radio link between two satellites rather than to the ground. |

---

## Software patterns used here

| Term | Where it shows up |
|---|---|
| **Strategy** | `IEphemeris` implementations — Kepler, fixed, tabulated, n-body |
| **State** | A spacecraft switching between *integrated* and *on rails* |
| **Game Loop / Fix Your Timestep** | `SimClock` and `StepPacer` |
| **Data locality (SoA)** | Structure-of-Arrays layout for thousands of satellites. Instead of 10,000 `Satellite` objects, keep 10,000 x-coordinates contiguously. The cache cares; below a few hundred bodies you should not. |
| **Component** | Spacecraft subsystems — power, comms, attitude control |
| **Event queue** | The telemetry bus, deterministic and replayable |
| **Flyweight** | One satellite-bus definition shared by every member of a constellation |
| **Object pool** | Satellites and debris, allocated once |

---

## Constants and conventions used throughout

| Symbol | Meaning |
|---|---|
| `GM` (or `μ`) | Standard gravitational parameter, `G × mass`, in m³/s². Measured far more precisely than `G` and `M` separately, so it is always the published quantity. |
| `AU` | Astronomical unit, 149,597,870,700 m exactly (defined, not measured). |
| **SI everywhere** | Internally, all quantities are metres, kilograms, seconds, radians. Kilometres, days, degrees and AU exist only at the boundary, for humans. |

The Mars Climate Orbiter was lost in 1999 because one team worked in
pound-force-seconds and another in newton-seconds. "SI internally, convert at
the edge" is not pedantry.
