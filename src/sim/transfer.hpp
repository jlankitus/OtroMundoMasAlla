// Interplanetary transfer planning: when to leave, and what it costs.
//
// Hohmann with instantaneous radii — the half-ellipse between the two orbits
// as they are right now, patched-conic style. Not a Lambert solver (that is
// the acknowledged next rung): this is the closed-form first approximation
// every mission design course starts from, and its numbers for Earth-Mars
// are famous enough to validate against by heart: ~259 days in transit,
// a ~44 degree phase angle at departure, ~2.9 km/s of v-infinity, and a
// ~3.6 km/s injection burn from low orbit — the Oberth effect making the
// deep-space delta-v cheaper when bought at periapsis.
#pragma once

#include "core/epoch.hpp"
#include "physics/solar_system.hpp"

namespace omma {

struct TransferPlan {
    bool   valid{false};
    double waitSeconds{0.0};          ///< from `now` until the window opens
    double transferSeconds{0.0};      ///< time of flight on the half-ellipse
    double phaseAngleDeg{0.0};        ///< required target lead angle at departure
    double currentPhaseAngleDeg{0.0}; ///< where the target actually is now
    double vInfinityMps{0.0};         ///< hyperbolic excess at departure
    /// The injection burn from a circular parking orbit of `parkingRadius`,
    /// via the Oberth-corrected escape: sqrt(v_inf^2 + 2 mu/r) - sqrt(mu/r).
    double departureDeltaVMps{0.0};
};

/// Plan the next Hohmann window from \p from to \p to as of \p now, departing
/// out of a circular parking orbit of \p parkingRadiusMetres around `from`.
[[nodiscard]] TransferPlan planHohmannTransfer(const SolarSystem& system,
                                               BodyId from, BodyId to, Epoch now,
                                               double parkingRadiusMetres);

}  // namespace omma
