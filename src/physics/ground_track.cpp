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

Vec3 surfacePosition(const LatLon& site, double bodyRadius,
                     double rotationAngle) noexcept {
    const double inertialLongitude = site.longitudeRadians + rotationAngle;
    const double cosLat = std::cos(site.latitudeRadians);
    return Vec3{bodyRadius * cosLat * std::cos(inertialLongitude),
                bodyRadius * cosLat * std::sin(inertialLongitude),
                bodyRadius * std::sin(site.latitudeRadians)};
}

Vec3 surfaceVelocity(const Vec3& surfacePos, double siderealPeriodSeconds) noexcept {
    if (!(siderealPeriodSeconds > 0.0)) {
        return Vec3::zero();
    }
    const double omega = constants::kTwoPi / siderealPeriodSeconds;
    // omega x r with omega = (0, 0, w).
    return Vec3{-omega * surfacePos.y, omega * surfacePos.x, 0.0};
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
