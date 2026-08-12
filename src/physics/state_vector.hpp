// ─────────────────────────────────────────────────────────────────────────────
// StateVector — position and velocity, the complete motion state of a point
// mass. Six numbers.
//
// Frame convention, relied on everywhere: unless a function name says
// otherwise, a StateVector is in the heliocentric ecliptic J2000 frame —
// origin the Sun, x toward the J2000 vernal equinox, z ecliptic north, y
// completing a right-handed set. The frame is inertial; integrating in a
// rotating frame without Coriolis/centrifugal terms is a classic bug.
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
