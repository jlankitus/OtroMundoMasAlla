// ─────────────────────────────────────────────────────────────────────────────
// IEphemeris — the environment: a body whose state is a KNOWN FUNCTION OF
// TIME, valid at any instant (random access in time), including the sub-step
// epochs an RK4 stage asks for. Spacecraft are integrated against this
// environment; an integrated body cannot answer sample(arbitrary t) and does
// not implement it. SPICE, GMAT, STK and Orekit draw the same line.
// See docs/DESIGN.md §2.
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

    /// GM, m^3/s^2 — measured directly, to far more digits than G and the
    /// mass are known separately.
    [[nodiscard]] virtual double gravitationalParameter() const noexcept = 0;

    /// Mean radius, metres. Used for collision, rendering and horizon checks.
    [[nodiscard]] virtual double meanRadius() const noexcept = 0;

    /// The J2 zonal harmonic — oblateness, dimensionless. Zero (the default)
    /// means the body is treated as a perfect sphere.
    [[nodiscard]] virtual double j2() const noexcept { return 0.0; }

    /// Equatorial radius J2 is defined against, metres.
    [[nodiscard]] virtual double equatorialRadius() const noexcept { return meanRadius(); }

    /// Sidereal rotation period, seconds. Zero (the default) means the body
    /// has no rotation model and therefore no ground track.
    [[nodiscard]] virtual double siderealRotationPeriod() const noexcept { return 0.0; }

    /// True when orbits here decay by drag. Only Earth for now: the density
    /// model is Earth-calibrated, so Mars stays false despite having air.
    [[nodiscard]] virtual bool hasAtmosphere() const noexcept { return false; }

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    IEphemeris() = default;
};

/// A body that never moves. The Sun, in a heliocentric frame.
///
/// The Sun really orbits the barycenter by about one solar radius; pinning it
/// at the origin defines the heliocentric frame, which is therefore not quite
/// inertial. The residual (~1e-8 of the Sun's field) is deliberately ignored.
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
