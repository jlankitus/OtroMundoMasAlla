# Game engine clients

The simulator is the product; every renderer is a client. The terminal app
proves that shape, and `src/ffi` makes it available to anything that can
load a shared library.

```
core → physics → sim → ffi (omma.dll / libomma.so, C ABI)
                        ├── apps/omma-unity   (P/Invoke, C#)
                        └── Unreal            (links the C++ targets or the C ABI)
```

## The boundary's rules

- **Pure C surface** (`src/ffi/omma_c.h`): opaque handle, POD structs, no
  exceptions across it. Bad input returns `0`/`NULL`, never crashes.
- **SI everywhere**: metres, seconds, radians, kg, in the sim's Z-up
  right-handed frame. Unit scale and handedness are presentation, so they
  belong to the client — exactly as pixels belonged to the terminal app.
  (Unity: swap y↔z, divide by metres-per-unit; see `OmmaSim.ToUnity`.)
- **Determinism survives the boundary.** `omma_step(n)` is the deterministic
  core; `omma_advance(dt, warp)` wraps it in the same pacer the terminal
  loop uses (whole steps, remainder carried, craft-aware budget, honest
  `omma_falling_behind`). An engine feeding ragged frame times still
  produces a replayable sequence of step counts.
- **No physics in the FFI.** Every function forwards to `sim`/`physics`
  (orbit polylines go through the same `perifocalBasis` the terminal
  renderer draws with), so the ABI cannot drift from the sim.
- `OMMA_ABI_VERSION` is bumped on any breaking change and checked by
  clients at startup — a stale DLL fails loudly.

## Precision: why clients must keep doubles until the last moment

The solar system is ~1e13 m across; a 32-bit float has ~7 significant
digits, so absolute float positions are off by kilometres at Jupiter. The
ABI therefore speaks doubles, and a client should subtract its focus/origin
in doubles *before* narrowing to floats (floating origin). Unity's
`SolarSystemView` treats the focused body as the origin for exactly this
reason.

## Unreal

Two options, both fine:

1. Link `omma.dll` through the same C ABI (portable, engine-agnostic).
2. Link `omma::sim` and friends as C++ static libraries directly — Unreal
   builds C++, so the FFI is optional there. The C ABI still earns its keep
   as the version firewall between engine updates and sim internals.

## Verification

`tests/test_ffi.cpp` exercises the ABI through the C surface only, and
`tools/ffi_smoke.py` drives the built DLL from python ctypes — if a
non-C++ runtime can create, advance, launch, burn and read honest delta-v
accounting back, any P/Invoke client can.
