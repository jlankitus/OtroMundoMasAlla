#include "sim/transfer.hpp"

#include "core/units.hpp"
#include "physics/kepler_ephemeris.hpp"
#include "physics/orbital_elements.hpp"

#include <cmath>

namespace omma {

TransferPlan planHohmannTransfer(const SolarSystem& system, BodyId from,
                                 BodyId to, Epoch now,
                                 double parkingRadiusMetres) {
    TransferPlan plan{};
    if (from == to || from == BodyId::Sun || to == BodyId::Sun) {
        return plan;
    }

    const auto& sun = system[BodyId::Sun];
    const auto& origin = system[from];
    const auto& target = system[to];
    const double muSun = sun.gravitationalParameter();

    const Vec3 r1Vec = origin.sample(now).position;
    const Vec3 r2Vec = target.sample(now).position;
    const double r1 = r1Vec.norm();
    const double r2 = r2Vec.norm();
    if (!(r1 > 0.0) || !(r2 > 0.0) || !(parkingRadiusMetres > 0.0)) {
        return plan;
    }

    // The half-ellipse: time of flight and the speed at its `from` end.
    const double aTransfer = 0.5 * (r1 + r2);
    plan.transferSeconds =
        constants::kPi * std::sqrt(aTransfer * aTransfer * aTransfer / muSun);
    const double vCircular = std::sqrt(muSun / r1);
    const double vDeparture =
        std::sqrt(muSun * (2.0 / r1 - 1.0 / aTransfer));
    plan.vInfinityMps = std::abs(vDeparture - vCircular);

    // Injection from the parking orbit, buying the excess at periapsis where
    // it is cheapest (Oberth): dv = sqrt(v_inf^2 + v_esc^2) - v_park.
    const double muOrigin = origin.gravitationalParameter();
    plan.departureDeltaVMps =
        std::sqrt(plan.vInfinityMps * plan.vInfinityMps
                  + 2.0 * muOrigin / parkingRadiusMetres)
        - std::sqrt(muOrigin / parkingRadiusMetres);

    // The window: the target must LEAD by pi minus what it sweeps during the
    // transfer (or lag, for inbound trips — the same formula, signed).
    //
    // Rates come from MEAN motion (semi-major axis), not today's radius:
    // Mars' eccentricity swings its instantaneous n by ~14%, and the window
    // wait extrapolates these rates across hundreds of days — the planner's
    // own self-check measured 43 degrees of miss before this distinction.
    const auto meanMotionOf = [&](const IEphemeris& body, double radius) {
        if (const auto* kepler = dynamic_cast<const KeplerEphemeris*>(&body)) {
            const double a = kepler->elementsAt(now).semiMajorAxis;
            return std::sqrt(muSun / (a * a * a));
        }
        return std::sqrt(muSun / (radius * radius * radius));
    };
    const double nTarget = meanMotionOf(target, r2);
    const double nOrigin = meanMotionOf(origin, r1);
    const double required = constants::kPi - nTarget * plan.transferSeconds;

    // Signed current phase of target ahead of origin, in the common prograde
    // (+z) direction.
    const double current = std::atan2(
        r1Vec.x * r2Vec.y - r1Vec.y * r2Vec.x,   // z of r1 x r2
        r1Vec.x * r2Vec.x + r1Vec.y * r2Vec.y);
    plan.phaseAngleDeg = toDegrees(wrapToPi(required));
    plan.currentPhaseAngleDeg = toDegrees(wrapToPi(current));

    // The phase drifts at (nTarget - nOrigin); wait until it equals the
    // required value, wrapping in the direction the drift actually moves.
    const double drift = nTarget - nOrigin;
    if (std::abs(drift) < 1e-12) {
        return plan;   // same orbit: no synodic window exists
    }
    double gap = wrapToTwoPi(required - current);
    if (drift < 0.0) {
        // Phase decreases over time (outbound case): the gap closes downward.
        gap = gap - constants::kTwoPi;
    }
    plan.waitSeconds = gap / drift;
    plan.valid = true;
    return plan;
}

}  // namespace omma
