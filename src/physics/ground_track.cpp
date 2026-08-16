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

double horizonAngularRadius(double bodyRadius, double orbitRadius) noexcept {
    if (!(orbitRadius > bodyRadius) || !(bodyRadius > 0.0)) {
        return 0.0;
    }
    return std::acos(bodyRadius / orbitRadius);
}

LatLon pointOnCircle(const LatLon& centre, double angularRadius,
                     double azimuth) noexcept {
    // Standard direct geodesic on a sphere.
    const double sinLat = std::sin(centre.latitudeRadians);
    const double cosLat = std::cos(centre.latitudeRadians);
    const double sinRadius = std::sin(angularRadius);
    const double cosRadius = std::cos(angularRadius);

    const double latitude = std::asin(sinLat * cosRadius
                                      + cosLat * sinRadius * std::cos(azimuth));
    const double longitude = centre.longitudeRadians
        + std::atan2(std::sin(azimuth) * sinRadius * cosLat,
                     cosRadius - sinLat * std::sin(latitude));
    return LatLon{latitude, wrapToPi(longitude)};
}

}  // namespace omma
