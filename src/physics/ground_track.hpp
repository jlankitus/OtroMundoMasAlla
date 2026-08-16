// Ground track: where a spacecraft is directly overhead on the rotating body
// beneath it. Latitude comes from geometry alone; longitude needs the body's
// rotation angle, so a body without a rotation model has no ground track.
//
// Same frame simplification as J2 (DESIGN.md §11): the body's equator lies in
// the reference plane and its rotation angle is zero at J2000 — good relative
// geometry (drift rates, repeat patterns, GEO station-keeping), not geodesy.
#pragma once

#include "core/epoch.hpp"
#include "core/vec3.hpp"

namespace omma {

struct LatLon {
    double latitudeRadians{0.0};    ///< +north, in [-pi/2, pi/2]
    double longitudeRadians{0.0};   ///< +east, wrapped to [-pi, pi)
};

/// Rotation angle of a body at \p t, radians, given its sidereal period.
/// Zero at J2000 by convention; returns 0 for a body with no rotation model
/// (siderealPeriodSeconds <= 0).
[[nodiscard]] double rotationAngleAt(double siderealPeriodSeconds, Epoch t) noexcept;

/// Subsatellite point for a craft at \p relativePosition from the body's
/// centre, with the body rotated to \p rotationAngle.
[[nodiscard]] LatLon subsatellitePoint(const Vec3& relativePosition,
                                       double rotationAngle) noexcept;

}  // namespace omma
