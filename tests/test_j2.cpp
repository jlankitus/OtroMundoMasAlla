// J2 — the oblateness perturbation, validated against the closed-form secular
// rates. The analytic result exists precisely so a numerical implementation
// can be checked against it:
//
//     dOmega/dt = -(3/2) n J2 (Req/p)^2 cos i        (nodal precession)
//
// A sun-synchronous orbit is the applied test: at 98.2 deg the node must walk
// eastward ~0.9856 deg/day — one full turn per year, tracking the Sun. That
// is not a coincidence; it is how the inclination was chosen.

#include "core/units.hpp"
#include "physics/ephemeris.hpp"
#include "physics/gravity_field.hpp"
#include "physics/integrator.hpp"
#include "physics/orbital_elements.hpp"
#include "sim/scenario.hpp"
#include "sim/world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace omma;

namespace {

constexpr double kGm = 3.98600435507e14;
constexpr double kJ2 = 1.08262668e-3;
constexpr double kEquatorialRadius = 6'378'137.0;

/// An Earth alone at the origin, oblate. Isolates the J2 term: any nodal
/// drift is J2, because nothing else in this field can produce one.
class OblateEarth final : public IEphemeris {
public:
    [[nodiscard]] StateVector sample(Epoch) const noexcept override { return {}; }
    [[nodiscard]] double gravitationalParameter() const noexcept override { return kGm; }
    [[nodiscard]] double meanRadius() const noexcept override { return 6.371e6; }
    [[nodiscard]] double j2() const noexcept override { return kJ2; }
    [[nodiscard]] double equatorialRadius() const noexcept override {
        return kEquatorialRadius;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "Earth"; }
};

/// Analytic secular nodal rate, rad/s.
double analyticNodalRate(double a, double e, double inclination) {
    const double n = std::sqrt(kGm / (a * a * a));
    const double p = a * (1.0 - e * e);
    const double ratio = kEquatorialRadius / p;
    return -1.5 * n * kJ2 * ratio * ratio * std::cos(inclination);
}

/// Integrate a circular orbit for \p seconds and return the measured change
/// in the longitude of the ascending node, radians.
double measuredNodeDrift(double altitude, double inclinationDeg, double seconds) {
    static const OblateEarth earth;
    GravityField field{{&earth}};

    OrbitalElements start{};
    start.semiMajorAxis = kEquatorialRadius + altitude;
    start.inclination = toRadians(inclinationDeg);
    start.longitudeOfAscendingNode = 0.7;   // away from the wrap point
    start.epoch = Epoch::j2000();

    StateVector state = stateFromElements(start, kGm, start.epoch);

    constexpr double dt = 5.0;
    const auto steps = static_cast<long long>(seconds / dt);
    for (long long i = 0; i < steps; ++i) {
        integrate(Integrator::RungeKutta4, state, field,
                  start.epoch + fromSeconds(dt * static_cast<double>(i)), dt);
    }

    const auto end = elementsFromState(
        state, kGm, start.epoch + fromSeconds(dt * static_cast<double>(steps)));
    return wrapToPi(end.longitudeOfAscendingNode - start.longitudeOfAscendingNode);
}

}  // namespace

TEST_CASE("nodal precession matches the analytic secular rate",
          "[physics][j2]") {
    // A 705 km sun-synchronous orbit, integrated for one day.
    constexpr double kAltitude = 705.0e3;
    constexpr double kInclinationDeg = 98.2;
    constexpr double kDay = 86'400.0;

    const double measured = measuredNodeDrift(kAltitude, kInclinationDeg, kDay);
    const double expected =
        analyticNodalRate(kEquatorialRadius + kAltitude, 0.0,
                          toRadians(kInclinationDeg)) * kDay;

    // The analytic rate is first-order secular theory; short-period J2 terms
    // ride on top of it, so agreement is a couple of percent, not exact.
    REQUIRE_THAT(measured, WithinRel(expected, 0.03));

    // And the headline number: ~+0.9856 deg/day, eastward, one lap per year.
    REQUIRE_THAT(toDegrees(measured), WithinAbs(0.9856, 0.05));
}

TEST_CASE("prograde orbits regress westward, retrograde precess eastward",
          "[physics][j2]") {
    constexpr double kQuarterDay = 21'600.0;
    // cos(51.6 deg) > 0: node moves west. cos(98.2 deg) < 0: node moves east.
    REQUIRE(measuredNodeDrift(400.0e3, 51.6, kQuarterDay) < 0.0);
    REQUIRE(measuredNodeDrift(705.0e3, 98.2, kQuarterDay) > 0.0);
}

TEST_CASE("J2 leaves the orbit's size and shape alone", "[physics][j2]") {
    // First-order secular J2 changes Omega and omega, not a and e. Integrate
    // a day and require the semi-major axis to have moved by metres, not km.
    static const OblateEarth earth;
    GravityField field{{&earth}};

    OrbitalElements start{};
    start.semiMajorAxis = kEquatorialRadius + 705.0e3;
    start.inclination = toRadians(98.2);
    start.epoch = Epoch::j2000();

    StateVector state = stateFromElements(start, kGm, start.epoch);
    constexpr double dt = 5.0;
    constexpr long long kSteps = 17'280;   // one day
    for (long long i = 0; i < kSteps; ++i) {
        integrate(Integrator::RungeKutta4, state, field,
                  start.epoch + fromSeconds(dt * static_cast<double>(i)), dt);
    }

    const auto end = elementsFromState(state, kGm,
                                       start.epoch + fromSeconds(dt * kSteps));
    // Short-period J2 oscillation in osculating a is ~ J2 * a ~ 8 km; the
    // bound catches secular growth while tolerating the oscillation.
    REQUIRE_THAT(end.semiMajorAxis, WithinRel(start.semiMajorAxis, 2.5e-3));
    REQUIRE(end.eccentricity < 0.01);
}

TEST_CASE("SUN-SYNC in the real world precesses like it is named",
          "[sim][j2]") {
    // Through the full stack: World, the standard solar system (Sun, Moon and
    // planets all pulling), the leo scenario's actual SUN-SYNC satellite.
    World world{SolarSystem::standard(), std::chrono::seconds{1}};
    applyScenario(world, Scenario::Leo);

    const Spacecraft& craft = world.spacecraft()[1];   // SUN-SYNC
    const double before = world.elementsOf(craft).longitudeOfAscendingNode;

    world.step(86'400);   // one simulated day

    const double drift =
        toDegrees(wrapToPi(world.elementsOf(craft).longitudeOfAscendingNode - before));

    // Third-body pulls and the finite step widen the tolerance vs the clean
    // two-body case above, but the year-long march must be unmistakable.
    REQUIRE_THAT(drift, WithinAbs(0.9856, 0.1));
}
