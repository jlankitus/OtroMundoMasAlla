/* ────────────────────────────────────────────────────────────────────────────
 * omma_c — the C ABI. What a game engine links against.
 *
 * Pure C: opaque handle, PODs, no exceptions across the boundary. Everything
 * is SI (metres, seconds, radians, kg) in the sim's Z-up right-handed frame;
 * unit scaling and handedness are the CLIENT's presentation concern, exactly
 * as pixels were the terminal renderer's. See docs/ENGINES.md.
 *
 * Determinism: omma_step(n) is the deterministic core. omma_advance() wraps
 * it with the same pacer logic the terminal app uses (whole steps, remainder
 * carried, craft-aware step budget) so an engine can feed ragged frame times
 * and stay on the fixed-step rails.
 * ──────────────────────────────────────────────────────────────────────────── */
#ifndef OMMA_C_H
#define OMMA_C_H

#include <stdint.h>

#if defined(_WIN32)
  #if defined(OMMA_FFI_EXPORTS)
    #define OMMA_API __declspec(dllexport)
  #else
    #define OMMA_API __declspec(dllimport)
  #endif
#else
  #define OMMA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any ABI-breaking change; clients should check it at startup. */
#define OMMA_ABI_VERSION 1

typedef struct OmmaSim OmmaSim;   /* opaque */

typedef struct OmmaVec3 {
    double x, y, z;
} OmmaVec3;

typedef struct OmmaBodyState {
    OmmaVec3 position;            /* metres, solar-system barycentric-ish frame */
    OmmaVec3 velocity;            /* m/s */
    double   radius;              /* mean radius, metres */
    double   gm;                  /* m^3/s^2 */
    int32_t  parentIndex;         /* -1 for the Sun */
} OmmaBodyState;

typedef struct OmmaCraftState {
    OmmaVec3 position;            /* metres */
    OmmaVec3 velocity;            /* m/s */
    int32_t  centralBodyIndex;
    int32_t  alive;               /* 0 dead */
    int32_t  burning;             /* engine currently lit */
    double   propellantKg;
    double   deltaVLeftMps;
    double   deltaVSpentMps;
} OmmaCraftState;

typedef struct OmmaElements {
    double semiMajorAxis;         /* metres */
    double eccentricity;
    double inclination;           /* radians */
    double longitudeOfAscendingNode;
    double argumentOfPeriapsis;
    double periodSeconds;         /* INFINITY for e >= 1 */
    double periapsisRadius;       /* metres, from the central body's centre */
    double apoapsisRadius;
} OmmaElements;

typedef struct OmmaLaunchRequest {
    const char* name;             /* NUL-terminated; NULL means "SAT" */
    int32_t aroundBodyIndex;
    double  altitudeMetres;
    double  eccentricity;
    double  inclinationRad;
    double  raanRad;
    double  argPeriapsisRad;
    double  meanAnomalyRad;
    double  dryMassKg;
    double  propellantKg;
    double  maxThrustNewtons;
    double  exhaustVelocityMps;
} OmmaLaunchRequest;

/* Matches omma::ThrustCommand::Frame. */
typedef enum OmmaBurnFrame {
    OMMA_BURN_PROGRADE = 0,
    OMMA_BURN_RETROGRADE = 1,
    OMMA_BURN_NORMAL = 2,
    OMMA_BURN_ANTINORMAL = 3
} OmmaBurnFrame;

/* ── lifecycle ─────────────────────────────────────────────────────────── */

OMMA_API int32_t  omma_abi_version(void);

/* scenario: "empty", "leo", "constellation" or "transfer" (as omma-ascii).
 * Returns NULL on an unknown scenario name. Start epoch is J2000 + startDays. */
OMMA_API OmmaSim* omma_create(const char* scenario, double startDays);
OMMA_API void     omma_destroy(OmmaSim* sim);

/* ── time ──────────────────────────────────────────────────────────────── */

/* Advance by a whole number of fixed 1 s steps derived from a wall-clock
 * frame delta and a warp factor. Remainder is carried, runaway frames are
 * clamped by a craft-aware budget. Returns the steps actually run. */
OMMA_API int64_t omma_advance(OmmaSim* sim, double frameDtSeconds, double warp);
/* The deterministic core: exactly n fixed steps. */
OMMA_API void    omma_step(OmmaSim* sim, int64_t n);
/* 1 if the last omma_advance hit the step budget (sim time falling behind). */
OMMA_API int32_t omma_falling_behind(const OmmaSim* sim);
OMMA_API double  omma_now_seconds_since_j2000(const OmmaSim* sim);

/* ── bodies ────────────────────────────────────────────────────────────── */

OMMA_API int32_t omma_body_count(const OmmaSim* sim);
/* Returns 0 on a bad index. */
OMMA_API int32_t omma_body_state(const OmmaSim* sim, int32_t index, OmmaBodyState* out);
/* Copies at most cap-1 chars plus a NUL. Returns the full name length. */
OMMA_API int32_t omma_body_name(const OmmaSim* sim, int32_t index, char* buf, int32_t cap);

/* ── spacecraft ────────────────────────────────────────────────────────── */

OMMA_API int32_t omma_craft_count(const OmmaSim* sim);
OMMA_API int32_t omma_craft_state(const OmmaSim* sim, int32_t index, OmmaCraftState* out);
OMMA_API int32_t omma_craft_name(const OmmaSim* sim, int32_t index, char* buf, int32_t cap);
OMMA_API int32_t omma_craft_elements(const OmmaSim* sim, int32_t index, OmmaElements* out);

/* Returns a craft id (> 0), or 0 if the requested orbit is impossible. */
OMMA_API int64_t omma_launch(OmmaSim* sim, const OmmaLaunchRequest* request);

/* A launch that starts ON THE PAD: at rest on the rotating surface, flown to
 * orbit by the ascent autopilot through the same physics as everything else.
 * Fields <= 0 fall back to the sim's rocket defaults; a NULL request means
 * "the default rocket from Canaveral, due east, 200 km". */
typedef struct OmmaSurfaceLaunchRequest {
    const char* name;              /* NULL -> "ASCENT" */
    int32_t bodyIndex;             /* body to launch from (3 = Earth) */
    double  latitudeRad;
    double  longitudeRad;
    double  targetAltitudeMetres;
    double  azimuthRad;            /* pi/2 = due east */
    double  dryMassKg;
    double  propellantKg;
    double  maxThrustNewtons;
    double  exhaustVelocityMps;
} OmmaSurfaceLaunchRequest;

OMMA_API int64_t omma_launch_ascent(OmmaSim* sim,
                                    const OmmaSurfaceLaunchRequest* request);

/* Ascent autopilot phase for a fleet index: 0 none/orbital, 1 vertical,
 * 2 pitch-over, 3 coast, 4 circularize, 5 done. -1 on a bad index. */
OMMA_API int32_t omma_craft_ascent_phase(const OmmaSim* sim, int32_t index);

/* Hohmann window from one body to another as of now. See sim/transfer.hpp
 * for the model and its honesty bounds. */
typedef struct OmmaTransferPlan {
    int32_t valid;
    double  waitSeconds;
    double  transferSeconds;
    double  phaseAngleDeg;
    double  currentPhaseAngleDeg;
    double  vInfinityMps;
    double  departureDeltaVMps;    /* injection burn from the parking orbit */
} OmmaTransferPlan;

OMMA_API int32_t omma_plan_transfer(const OmmaSim* sim, int32_t fromBody,
                                    int32_t toBody, double parkingRadiusMetres,
                                    OmmaTransferPlan* out);
/* Craft id for a fleet index, 0 on a bad index. Ids stay valid across steps. */
OMMA_API int64_t omma_craft_id(const OmmaSim* sim, int32_t index);
/* Returns 1 if the burn was accepted (refused when unaffordable or dead). */
OMMA_API int32_t omma_command_delta_v(OmmaSim* sim, int64_t craftId,
                                      int32_t frame, double deltaVMps);
OMMA_API void    omma_cancel_burn(OmmaSim* sim, int64_t craftId);

/* ── orbit polylines, for line renderers ───────────────────────────────── */

/* Sample the closed orbit of body `index` (its osculating ellipse around its
 * parent) at `count` points, world frame, uniformly in eccentric anomaly —
 * the same spacing the terminal renderer draws. Writes count*3 doubles
 * (x,y,z per point). Returns the points written (0 for the Sun / bad index). */
OMMA_API int32_t omma_body_orbit(const OmmaSim* sim, int32_t index,
                                 double* xyz, int32_t count);
/* Same, for a spacecraft's predicted path around its central body. */
OMMA_API int32_t omma_craft_orbit(const OmmaSim* sim, int32_t index,
                                  double* xyz, int32_t count);

/* Subsatellite point on the craft's central body, radians (+N, +E wrapped to
 * [-pi, pi)). Longitude is body-fixed only for bodies with a rotation model
 * (Earth, Mars); otherwise it is inertial. Returns 0 on a bad index. */
OMMA_API int32_t omma_craft_ground_track(const OmmaSim* sim, int32_t index,
                                         double* latitudeRad, double* longitudeRad);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* OMMA_C_H */
