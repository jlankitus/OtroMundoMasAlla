# omma-unity — the simulator in Unity

The same deterministic sim the terminal renders, drawn by Unity instead.
Unity is a *client*: it P/Invokes `omma.dll` through the C ABI in
`src/ffi/omma_c.h` and never touches sim internals. Same clock, same
physics, same scenarios — `leo` here is byte-for-byte the `leo` the QA
harness asserts on.

## Setup (once)

1. Build the DLL: `cmake --build --preset windows --config Release`
   → `build/windows/bin/Release/omma.dll`
2. In Unity Hub, create a new **3D (Built-In or URP)** project, Unity 6.
3. Copy `Assets/Omma/` from this folder into the project's `Assets/`.
4. Copy `omma.dll` into the project at `Assets/Plugins/x86_64/omma.dll`.
5. In an empty scene: create an empty GameObject, add **SolarSystemView**,
   put the camera somewhere like `(0, 25, -40)` looking at the origin,
   and press **Play**.

Three satellites appear in orbit around a to-scale Earth with orbit lines,
exactly like the terminal default.

| key | |
|---|---|
| `-` `=` | time warp down / up |
| `tab` | cycle the focus body (the floating origin) |
| `L` | launch a satellite around the focused body |
| `.` `,` | burn the newest craft prograde / retrograde — watch its line deform |

## The two ideas worth knowing

**Floating origin.** The solar system is 1e13 m across and a `float` loses
whole metres by 1e11, so nothing is ever placed in absolute coordinates:
every position is measured *relative to the focused body* in doubles first,
and only the small difference is handed to Unity as floats. Tab changes
which body is the origin. (The terminal camera works the same way.)

**Axis swap.** The sim is Z-up right-handed (astronomy convention); Unity is
Y-up left-handed. Swapping y↔z converts both at once and lays the ecliptic
on Unity's ground plane. All of it lives in `OmmaSim.ToUnity` — nothing else
in the project knows the sim's frame.

## Updating the DLL

Rebuild with CMake and re-copy `omma.dll` (Unity must be closed, or the
editor holds a lock on the old one). `omma_abi_version()` is checked at
startup, so a stale DLL fails loudly instead of drifting quietly.
