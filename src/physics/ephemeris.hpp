// ─────────────────────────────────────────────────────────────────────────────
// IEphemeris — the environment.
//
// A body whose state is a KNOWN FUNCTION OF TIME. Ask it for any instant and
// it answers immediately: five years from now, three days ago, or 0.5 seconds
// into the middle of an integrator step.
//
// WHY THIS IS A SEPARATE CONCEPT FROM AN INTEGRATED BODY
// The obvious design is one interface with a stateAt(t) method that everything
// implements. It does not survive contact with the physics, because the two
// kinds of body have genuinely different capabilities in time:
//
//                            IEphemeris        integrated body
//   "where at t = +5 years"  one call          must step all the way there
//   "where 3 days ago"       one call          gone, unless it was stored
//   access pattern           random            strictly sequential
//
// An integrated body physically cannot answer sample(arbitrary t). Forcing it
// behind this interface produces a method that throws or lies, and every
// caller ends up needing to know which kind it holds anyway.
//
// So the split is drawn where the physics draws it. Ephemerides are the
// environment; spacecraft are integrated against that environment. That
// separation is what real flight-dynamics tools do — SPICE, GMAT, STK and
// Orekit all treat planetary positions as a lookup, never as a simulation.
//
// AND IT IS WHY RK4 WORKS AT ALL
// RK4 evaluates acceleration four times per step, at t, t+h/2, t+h/2 and t+h.
// It will ask where Jupiter was half a second into the current step. An
// ephemeris answers exactly and for free. A fully integrated solar system
// would have to march every planet through the same four stages in lockstep,
// holding four copies of the entire system state, and any slip in that
// bookkeeping is a silent accuracy loss nobody notices for months.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/state_vector.hpp"

#include <string_view>

namespace omma {

class IEphemeris {
public:
    virtual ~IEphemeris() = default;

    IEphemeris(const IEphemeris&) = delete;
    IEphemeris& operator=(const IEphemeris&) = delete;

    /// State at \p t, in the root frame (heliocentric ecliptic J2000).
    ///
    /// Must be valid for ANY t, including instants between integrator steps.
    /// Implementations are expected to be cheap; this is called several times
    /// per body per step.
    [[nodiscard]] virtual StateVector sample(Epoch t) const noexcept = 0;

    /// GM, m^3/s^2. The quantity that actually matters for gravity, and the
    /// one that is published — it is measured by watching things orbit, to far
    /// more digits than G and the mass are known separately.
    [[nodiscard]] virtual double gravitationalParameter() const noexcept = 0;

    /// Mean radius, metres. Used for collision, rendering and horizon checks.
    [[nodiscard]] virtual double meanRadius() const noexcept = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    IEphemeris() = default;
};

/// A body that never moves. The Sun, in a heliocentric frame.
///
/// Strictly the Sun does move — it orbits the Solar System barycenter, mostly
/// under Jupiter's influence, by about one solar radius. Pinning it at the
/// origin is the definition of a heliocentric frame rather than an error, but
/// it does mean this frame is not perfectly inertial. The residual
/// acceleration is ~1e-8 of the Sun's own field and is ignored, deliberately.
class FixedEphemeris final : public IEphemeris {
public:
    FixedEphemeris(std::string_view name, double gm, double meanRadius,
                   Vec3 position = Vec3::zero()) noexcept
        : name_{name}, gm_{gm}, meanRadius_{meanRadius}, position_{position} {}

    [[nodiscard]] StateVector sample(Epoch) const noexcept override {
        return StateVector{position_, Vec3::zero()};
    }
    [[nodiscard]] double gravitationalParameter() const noexcept override { return gm_; }
    [[nodiscard]] double meanRadius() const noexcept override { return meanRadius_; }
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

private:
    std::string_view name_;
    double gm_;
    double meanRadius_;
    Vec3 position_;
};

}  // namespace omma
