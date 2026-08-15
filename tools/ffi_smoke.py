#!/usr/bin/env python3
"""Drive omma.dll through ctypes — the ABI proven from outside C++.

If python can create a world, advance it, launch, burn, and read honest
delta-v accounting back through the C surface, then any P/Invoke-style
client (Unity, C#, Rust, whatever) can. Run it against a built DLL:

    python tools/ffi_smoke.py build/windows/bin/Release/omma.dll
"""

from __future__ import annotations

import ctypes as C
import math
import sys


class Vec3(C.Structure):
    _fields_ = [("x", C.c_double), ("y", C.c_double), ("z", C.c_double)]


class BodyState(C.Structure):
    _fields_ = [("position", Vec3), ("velocity", Vec3), ("radius", C.c_double),
                ("gm", C.c_double), ("parentIndex", C.c_int32)]


class CraftState(C.Structure):
    _fields_ = [("position", Vec3), ("velocity", Vec3),
                ("centralBodyIndex", C.c_int32), ("alive", C.c_int32),
                ("burning", C.c_int32), ("propellantKg", C.c_double),
                ("deltaVLeftMps", C.c_double), ("deltaVSpentMps", C.c_double)]


class Elements(C.Structure):
    _fields_ = [("semiMajorAxis", C.c_double), ("eccentricity", C.c_double),
                ("inclination", C.c_double), ("raan", C.c_double),
                ("argPeriapsis", C.c_double), ("periodSeconds", C.c_double),
                ("periapsisRadius", C.c_double), ("apoapsisRadius", C.c_double)]


class LaunchRequest(C.Structure):
    _fields_ = [("name", C.c_char_p), ("aroundBodyIndex", C.c_int32),
                ("altitudeMetres", C.c_double), ("eccentricity", C.c_double),
                ("inclinationRad", C.c_double), ("raanRad", C.c_double),
                ("argPeriapsisRad", C.c_double), ("meanAnomalyRad", C.c_double),
                ("dryMassKg", C.c_double), ("propellantKg", C.c_double),
                ("maxThrustNewtons", C.c_double), ("exhaustVelocityMps", C.c_double)]


def main() -> int:
    dll_path = sys.argv[1] if len(sys.argv) > 1 else "build/windows/bin/Release/omma.dll"
    lib = C.CDLL(dll_path)

    lib.omma_create.restype = C.c_void_p
    lib.omma_create.argtypes = [C.c_char_p, C.c_double]
    lib.omma_advance.restype = C.c_int64
    lib.omma_advance.argtypes = [C.c_void_p, C.c_double, C.c_double]
    lib.omma_launch.restype = C.c_int64
    lib.omma_launch.argtypes = [C.c_void_p, C.POINTER(LaunchRequest)]
    lib.omma_command_delta_v.argtypes = [C.c_void_p, C.c_int64, C.c_int32, C.c_double]
    lib.omma_step.argtypes = [C.c_void_p, C.c_int64]
    lib.omma_destroy.argtypes = [C.c_void_p]

    checks = 0

    def check(claim: str, ok: bool) -> None:
        nonlocal checks
        checks += 1
        print(f"  {'ok' if ok else 'FAIL'}  {claim}")
        if not ok:
            sys.exit(1)

    print(f"omma.dll ABI v{lib.omma_abi_version()}")

    sim = lib.omma_create(b"leo", C.c_double(9350.0))
    check("leo scenario creates", sim is not None)
    check("eleven bodies", lib.omma_body_count(C.c_void_p(sim)) == 11)
    check("three craft on open", lib.omma_craft_count(C.c_void_p(sim)) == 3)

    name = C.create_string_buffer(32)
    lib.omma_craft_name(C.c_void_p(sim), 0, name, 32)
    check("first craft is ISS-LIKE", name.value == b"ISS-LIKE")

    steps = sum(lib.omma_advance(C.c_void_p(sim), 1.0 / 30.0, 3600.0)
                for _ in range(30))
    check("one second of frames at 1 hour/s = 3600 steps", steps == 3600)

    request = LaunchRequest(name=b"PY-SAT", aroundBodyIndex=3,
                            altitudeMetres=500e3, inclinationRad=0.5,
                            dryMassKg=200.0, propellantKg=80.0,
                            maxThrustNewtons=400.0, exhaustVelocityMps=2200.0)
    craft_id = lib.omma_launch(C.c_void_p(sim), C.byref(request))
    check("python can launch a satellite", craft_id != 0)

    before = Elements()
    lib.omma_craft_elements(C.c_void_p(sim), 3, C.byref(before))
    check("burn accepted", lib.omma_command_delta_v(
        C.c_void_p(sim), craft_id, 0, C.c_double(75.0)) == 1)
    lib.omma_step(C.c_void_p(sim), 600)

    state = CraftState()
    lib.omma_craft_state(C.c_void_p(sim), 3, C.byref(state))
    check("exactly 75 m/s spent", math.isclose(state.deltaVSpentMps, 75.0, rel_tol=1e-3))

    after = Elements()
    lib.omma_craft_elements(C.c_void_p(sim), 3, C.byref(after))
    check("apoapsis climbed", after.apoapsisRadius > before.apoapsisRadius + 100e3)
    check("periapsis held", math.isclose(after.periapsisRadius,
                                         before.periapsisRadius, rel_tol=0.01))

    points = (C.c_double * (64 * 3))()
    got = lib.omma_craft_orbit(C.c_void_p(sim), 3, points, 64)
    check("orbit polyline sampled", got == 64
          and all(math.isfinite(v) for v in points))

    lib.omma_destroy(C.c_void_p(sim))
    print(f"{checks}/{checks} — the ABI works from a non-C++ client")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
