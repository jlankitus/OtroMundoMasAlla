// ─────────────────────────────────────────────────────────────────────────────
// StateVector — position and velocity, the complete motion state of a point
// mass. Six numbers.
//
// FRAME CONVENTION, stated once and relied on everywhere
// Unless a function name says otherwise, a StateVector is expressed in the
// heliocentric ecliptic J2000 frame:
//
//   origin  the Solar System's central body (the Sun)
//   x-axis  toward the vernal equinox of J2000
//   z-axis  ecliptic north (normal to Earth's orbital plane at J2000)
//   y-axis  completes a right-handed set
//
// This is an *inertial* frame: it does not rotate. Newton's laws only hold in
// one of those, so integrating in a rotating frame without adding Coriolis and
// centrifugal terms is a classic and very confusing bug.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/vec3.hpp"

namespace omma {

struct StateVector {
    Vec3 position;   ///< metres
    Vec3 velocity;   ///< metres per second

    [[nodiscard]] constexpr StateVector relativeTo(const StateVector& origin) const noexcept {
        return {position - origin.position, velocity - origin.velocity};
    }

    [[nodiscard]] constexpr bool isFinite() const noexcept {
        return position.isFinite() && velocity.isFinite();
    }

    friend constexpr bool operator==(const StateVector&, const StateVector&) noexcept = default;
};

[[nodiscard]] constexpr StateVector operator+(const StateVector& a, const StateVector& b) noexcept {
    return {a.position + b.position, a.velocity + b.velocity};
}

[[nodiscard]] constexpr StateVector operator-(const StateVector& a, const StateVector& b) noexcept {
    return {a.position - b.position, a.velocity - b.velocity};
}

}  // namespace omma
