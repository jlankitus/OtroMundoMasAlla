// Ground tracks, validated by the orbits whose tracks are known by heart:
// a geostationary satellite's track is a point, an ISS-inclination track is a
// sinusoid bounded by its inclination that walks west a fixed amount per
// revolution. Both fall out of latitude/longitude being RIGHT, and fail if
// either the rotation model or the subsatellite math is wrong.

#include "core/units.hpp"
#include "physics/ground_track.hpp"
#include "sim/scenario.hpp"
#include "sim/world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace omma;
using namespace omma::literals;
using namespace std::chrono_literals;

namespace {

constexpr double kSiderealDay = 86'164.0905;

World makeEarthWorld() {
    return World{SolarSystem::standard(), std::chrono::seconds{1}};
}

/// A geostationary-ish launch: semi-major axis chosen so the orbital period
/// equals Earth's sidereal day, equatorial, circular.
LaunchRequest geoRequest() {
    LaunchRequest request{};
    request.name = "GEO";
    request.aroundBody = BodyId::Earth;
    // a for a sidereal-day period: a = (GM (T/2pi)^2)^(1/3) ~ 42,164 km.
    request.altitudeMetres = 42.164e6 - 6.371e6;
    request.inclinationRadians = 0.0;
    return request;
}

}  // namespace

TEST_CASE("the subsatellite point math is exact where it can be checked",
          "[physics][groundtrack]") {
    // Over the pole: latitude 90, longitude irrelevant but defined.
    const auto pole = subsatellitePoint(Vec3{0.0, 0.0, 7.0e6}, 0.0);
    REQUIRE_THAT(toDegrees(pole.latitudeRadians), WithinAbs(90.0, 1e-9));

    // On the +x axis with the body unrotated: (0 N, 0 E).
    const auto origin = subsatellitePoint(Vec3{7.0e6, 0.0, 0.0}, 0.0);
    REQUIRE_THAT(origin.latitudeRadians, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(origin.longitudeRadians, WithinAbs(0.0, 1e-12));

    // Rotate the body a quarter turn east: the same inertial point is now a
    // quarter turn WEST in body-fixed longitude.
    const auto rotated = subsatellitePoint(Vec3{7.0e6, 0.0, 0.0},
                                           constants::kPi / 2.0);
    REQUIRE_THAT(toDegrees(rotated.longitudeRadians), WithinAbs(-90.0, 1e-9));

    // A body with no rotation model reports angle zero at any epoch.
    REQUIRE(rotationAngleAt(0.0, Epoch::j2000() + 12h) == 0.0);
}

TEST_CASE("a geostationary satellite's ground track stands still",
          "[sim][groundtrack]") {
    World world = makeEarthWorld();
    const auto id = world.launch(geoRequest());
    REQUIRE(id.isValid());

    const auto start = world.groundTrackOf(*world.find(id));

    // A quarter of a day at a time, one full sidereal day total. J2 and lunar
    // perturbation nudge a real GEO slot, so "stands still" means within a
    // fraction of a degree, not exactly.
    double maxDriftDeg = 0.0;
    for (int quarter = 0; quarter < 4; ++quarter) {
        world.step(static_cast<std::int64_t>(kSiderealDay / 4.0));
        const auto now = world.groundTrackOf(*world.find(id));
        maxDriftDeg = std::max(maxDriftDeg,
                               std::abs(toDegrees(wrapToPi(now.longitudeRadians
                                                           - start.longitudeRadians))));
    }
    REQUIRE(maxDriftDeg < 0.75);
    REQUIRE_THAT(toDegrees(world.groundTrackOf(*world.find(id)).latitudeRadians),
                 WithinAbs(0.0, 0.5));
}

TEST_CASE("an inclined track is bounded by its inclination and walks west",
          "[sim][groundtrack]") {
    World world = makeEarthWorld();
    applyScenario(world, Scenario::Leo);
    const Spacecraft& iss = world.spacecraft()[0];   // ISS-LIKE, 51.6 deg

    // Sample one orbit finely: latitude must never exceed the inclination.
    double maxLatDeg = 0.0;
    const double period = 92.4 * 60.0;
    const auto sampleStep = static_cast<std::int64_t>(period / 64.0);
    for (int i = 0; i < 64; ++i) {
        world.step(sampleStep);
        maxLatDeg = std::max(maxLatDeg,
                             std::abs(toDegrees(world.groundTrackOf(iss).latitudeRadians)));
    }
    REQUIRE(maxLatDeg < 51.6 + 0.5);
    REQUIRE(maxLatDeg > 45.0);   // and it actually reaches high latitude

    // Successive ascending equator crossings shift west by the ground the
    // Earth covers in one orbit: 360 deg * period / sidereal day ~ 23.2 deg.
    // Track longitude at matching orbit phase, one full orbit apart.
    const double lonA = toDegrees(world.groundTrackOf(iss).longitudeRadians);
    world.step(static_cast<std::int64_t>(period));
    const double lonB = toDegrees(world.groundTrackOf(iss).longitudeRadians);

    const double shift = toDegrees(wrapToPi(toRadians(lonB - lonA)));
    const double expected = -360.0 * period / kSiderealDay;
    // Same-phase comparison absorbs most orbit-shape effects; J2 and the
    // orbit's own nodal motion leave a degree or two.
    REQUIRE_THAT(shift, WithinAbs(expected, 2.0));
}
