#include "physics/ground_track.hpp"

#include "core/units.hpp"
#include "physics/orbital_elements.hpp"

#include <cmath>

namespace omma {

double rotationAngleAt(double siderealPeriodSeconds, Epoch t) noexcept {
    if (!(siderealPeriodSeconds > 0.0)) {
        return 0.0;
    }
    const double rate = constants::kTwoPi / siderealPeriodSeconds;
    return wrapToTwoPi(rate * t.secondsSinceJ2000());
}

LatLon subsatellitePoint(const Vec3& relativePosition, double rotationAngle) noexcept {
    const double r = relativePosition.norm();
    if (!(r > 0.0)) {
        return {};
    }
    // Latitude from the z component; inertial longitude from x,y; body-fixed
    // longitude by subtracting how far the ground has turned underneath.
    const double latitude = std::asin(relativePosition.z / r);
    const double inertialLongitude = std::atan2(relativePosition.y, relativePosition.x);
    return LatLon{latitude, wrapToPi(inertialLongitude - rotationAngle)};
}

}  // namespace omma
