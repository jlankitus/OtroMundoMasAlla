// Round-tripping between orbital elements and state vectors, plus the
// conservation laws that any correct implementation must satisfy.
//
// The conservation tests matter more than the round trips. A round trip is
// happy with two mistakes that cancel; energy and angular momentum being
// constant along an orbit is an independent physical fact that a broken
// implementation has no way to fake.

#include "core/units.hpp"
#include "physics/orbital_elements.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using namespace std::chrono_literals;
using namespace omma::literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::Epoch;
using omma::OrbitalElements;
using omma::StateVector;
using omma::Vec3;

namespace {

constexpr double kGmEarth = 3.98600435507e14;

/// A spread of orbits covering every case the conversion has a branch for.
struct NamedOrbit {
    const char* name;
    OrbitalElements elements;
};

std::vector<NamedOrbit> testOrbits() {
    auto make = [](double aKm, double e, double iDeg, double raanDeg,
                   double argpDeg, double m0Deg) {
        OrbitalElements el{};
        el.semiMajorAxis = aKm * 1000.0;
        el.eccentricity = e;
        el.inclination = omma::toRadians(iDeg);
        el.longitudeOfAscendingNode = omma::toRadians(raanDeg);
        el.argumentOfPeriapsis = omma::toRadians(argpDeg);
        el.meanAnomalyAtEpoch = omma::toRadians(m0Deg);
        el.epoch = Epoch::j2000();
        return el;
    };

    return {
        {"LEO, inclined",        make(6771,   0.0012, 51.6,  120.0,  45.0,  10.0)},
        {"ISS-like",             make(6795,   0.0004, 51.64,  33.0, 130.0, 220.0)},
        {"sun-synchronous",      make(7078,   0.001,  98.2,  340.0,  90.0, 180.0)},
        {"GTO, very eccentric",  make(24582,  0.73,    7.0,   15.0, 178.0,  30.0)},
        {"Molniya",              make(26600,  0.74,   63.4,   90.0, 270.0, 100.0)},
        {"geostationary",        make(42164,  0.0002,  0.05,  10.0,  20.0, 300.0)},
        {"polar circular",       make(7000,   0.0,    90.0,   45.0,   0.0, 200.0)},
        {"retrograde",           make(8000,   0.05,  145.0,  200.0,  60.0,  75.0)},
        // The degenerate cases the conversion has explicit branches for.
        {"exactly circular",     make(7500,   0.0,    28.5,   77.0,   0.0, 123.0)},
        {"exactly equatorial",   make(9000,   0.2,     0.0,    0.0,  95.0, 250.0)},
        {"circular equatorial",  make(11000,  0.0,     0.0,    0.0,   0.0,  47.0)},
    };
}

}  // namespace

TEST_CASE("elements -> state -> elements round-trips", "[physics][elements]") {
    // Compared through the state vector rather than element by element,
    // because the degenerate cases legitimately return *different* elements
    // that describe the *same* orbit. A circular orbit has no periapsis, so
    // any argument of periapsis is as good as another; what must be preserved
    // is where the body actually is.
    for (const auto& [name, original] : testOrbits()) {
        INFO(name);
        const StateVector state = stateFromElements(original, kGmEarth, original.epoch);
        const OrbitalElements recovered = elementsFromState(state, kGmEarth, original.epoch);
        const StateVector again = stateFromElements(recovered, kGmEarth, original.epoch);

        REQUIRE_THAT(again.position.x, WithinRel(state.position.x, 1e-9)
                                           || WithinAbs(state.position.x, 1e-3));
        REQUIRE_THAT(again.position.y, WithinRel(state.position.y, 1e-9)
                                           || WithinAbs(state.position.y, 1e-3));
        REQUIRE_THAT(again.position.z, WithinRel(state.position.z, 1e-9)
                                           || WithinAbs(state.position.z, 1e-3));
        REQUIRE_THAT(again.velocity.x, WithinRel(state.velocity.x, 1e-9)
                                           || WithinAbs(state.velocity.x, 1e-9));
        REQUIRE_THAT(again.velocity.y, WithinRel(state.velocity.y, 1e-9)
                                           || WithinAbs(state.velocity.y, 1e-9));
        REQUIRE_THAT(again.velocity.z, WithinRel(state.velocity.z, 1e-9)
                                           || WithinAbs(state.velocity.z, 1e-9));
    }
}

TEST_CASE("shape elements survive the round trip directly", "[physics][elements]") {
    // Semi-major axis and eccentricity are well defined even for the
    // degenerate orbits, so these can be compared without going through a
    // state vector.
    for (const auto& [name, original] : testOrbits()) {
        INFO(name);
        const StateVector state = stateFromElements(original, kGmEarth, original.epoch);
        const OrbitalElements recovered = elementsFromState(state, kGmEarth, original.epoch);

        REQUIRE_THAT(recovered.semiMajorAxis, WithinRel(original.semiMajorAxis, 1e-10));
        REQUIRE_THAT(recovered.eccentricity,
                     WithinAbs(original.eccentricity, 1e-10));
        REQUIRE_THAT(recovered.inclination,
                     WithinAbs(original.inclination, 1e-10));
    }
}

TEST_CASE("energy and angular momentum are conserved along an orbit",
          "[physics][elements][conservation]") {
    // Sample each orbit at many points around one full revolution and require
    // the two conserved quantities to stay put. This is the independent check:
    // it cannot be satisfied by a rotation matrix that is transposed, or an
    // anomaly conversion with a sign error, even though a round trip can be.
    for (const auto& [name, elements] : testOrbits()) {
        INFO(name);
        const double period = orbitalPeriod(elements, kGmEarth);
        const StateVector reference = stateFromElements(elements, kGmEarth, elements.epoch);
        const double energy0 = specificOrbitalEnergy(reference, kGmEarth);
        const Vec3 h0 = specificAngularMomentum(reference);

        for (int i = 1; i <= 64; ++i) {
            const double fraction = static_cast<double>(i) / 64.0;
            const Epoch t = elements.epoch + omma::fromSeconds(period * fraction);
            const StateVector s = stateFromElements(elements, kGmEarth, t);

            REQUIRE_THAT(specificOrbitalEnergy(s, kGmEarth), WithinRel(energy0, 1e-11));
            REQUIRE_THAT(specificAngularMomentum(s).x, WithinAbs(h0.x, 1e-11 * h0.norm()));
            REQUIRE_THAT(specificAngularMomentum(s).y, WithinAbs(h0.y, 1e-11 * h0.norm()));
            REQUIRE_THAT(specificAngularMomentum(s).z, WithinAbs(h0.z, 1e-11 * h0.norm()));
        }
    }
}

TEST_CASE("an orbit returns exactly to its starting state after one period",
          "[physics][elements][conservation]") {
    // Closure. Not approximately closed — a Kepler propagator has no
    // accumulated error to leak, so after exactly one period the position must
    // agree to within a millimetre out of thousands of kilometres.
    for (const auto& [name, elements] : testOrbits()) {
        INFO(name);
        const double period = orbitalPeriod(elements, kGmEarth);
        const StateVector start = stateFromElements(elements, kGmEarth, elements.epoch);
        const StateVector after =
            stateFromElements(elements, kGmEarth, elements.epoch + omma::fromSeconds(period));

        REQUIRE_THAT(distance(start.position, after.position),
                     WithinAbs(0.0, 1e-3));   // millimetres, on a ~7000 km orbit
    }
}

TEST_CASE("periapsis and apoapsis bound the orbit", "[physics][elements]") {
    for (const auto& [name, elements] : testOrbits()) {
        INFO(name);
        const double rp = periapsisRadius(elements);
        const double ra = apoapsisRadius(elements);
        const double period = orbitalPeriod(elements, kGmEarth);

        double minR = std::numeric_limits<double>::max();
        double maxR = 0.0;
        for (int i = 0; i < 512; ++i) {
            const double fraction = static_cast<double>(i) / 512.0;
            const Epoch t = elements.epoch + omma::fromSeconds(period * fraction);
            const double r = stateFromElements(elements, kGmEarth, t).position.norm();
            minR = std::min(minR, r);
            maxR = std::max(maxR, r);
        }

        // The hard invariant: no sample may fall outside the analytic bounds.
        // This is a property of the model and is asserted to full precision.
        REQUIRE(minR >= rp * (1.0 - 1e-9));
        REQUIRE(maxR <= ra * (1.0 + 1e-9));

        // The soft check: the samples should get close to those bounds. The
        // tolerance here measures the TEST, not the model. Sampling uniformly
        // in time under-resolves periapsis, because that is precisely where
        // the body is moving fastest -- for the e = 0.74 Molniya orbit, 512
        // samples put the nearest one about 0.3% above true periapsis. Worth
        // knowing which of your tolerances describe the physics and which
        // describe your own sampling.
        REQUIRE_THAT(minR, WithinRel(rp, 1e-2));
        REQUIRE_THAT(maxR, WithinRel(ra, 1e-3));
    }
}

TEST_CASE("vis-viva holds at every point on the orbit", "[physics][elements][conservation]") {
    // v^2 = GM (2/r - 1/a). An independent textbook relation between speed and
    // radius that the implementation never uses directly, so it is a genuine
    // outside check rather than a restatement of the code.
    const OrbitalElements el = testOrbits()[3].elements;   // the eccentric GTO
    const double period = orbitalPeriod(el, kGmEarth);

    for (int i = 0; i < 128; ++i) {
        const Epoch t = el.epoch + omma::fromSeconds(period * static_cast<double>(i) / 128.0);
        const StateVector s = stateFromElements(el, kGmEarth, t);

        const double predicted =
            kGmEarth * (2.0 / s.position.norm() - 1.0 / el.semiMajorAxis);
        REQUIRE_THAT(s.velocity.normSquared(), WithinRel(predicted, 1e-10));
    }
}

TEST_CASE("a circular orbit has constant speed and radius", "[physics][elements]") {
    OrbitalElements el{};
    el.semiMajorAxis = 7000.0_km;
    el.eccentricity = 0.0;
    el.inclination = 45.0_deg;
    el.epoch = Epoch::j2000();

    const double period = orbitalPeriod(el, kGmEarth);
    const double expectedSpeed = std::sqrt(kGmEarth / el.semiMajorAxis);

    for (int i = 0; i < 64; ++i) {
        const Epoch t = el.epoch + omma::fromSeconds(period * static_cast<double>(i) / 64.0);
        const StateVector s = stateFromElements(el, kGmEarth, t);
        REQUIRE_THAT(s.position.norm(), WithinRel(el.semiMajorAxis, 1e-12));
        REQUIRE_THAT(s.velocity.norm(), WithinRel(expectedSpeed, 1e-12));
        // And velocity is perpendicular to position, which is what "circular"
        // means geometrically.
        REQUIRE_THAT(dot(s.position, s.velocity),
                     WithinAbs(0.0, 1e-6 * s.position.norm() * s.velocity.norm()));
    }
}

TEST_CASE("orbital period matches the textbook formula", "[physics][elements]") {
    // A 400 km circular orbit takes about 92.6 minutes; geostationary takes
    // one sidereal day. Both are numbers you can look up, which makes them
    // worth asserting.
    //
    // "400 km altitude" is ambiguous until you say above WHAT. The textbook
    // 92.56 min uses the equatorial radius, 6378 km. Using the mean radius,
    // 6371 km, gives 92.41 min. Seven kilometres of definition is nine seconds
    // of period -- small, but this is exactly the class of quiet mismatch that
    // makes two correct programs disagree.
    OrbitalElements leo{};
    leo.semiMajorAxis = 6378.0_km + 400.0_km;
    REQUIRE_THAT(orbitalPeriod(leo, kGmEarth) / 60.0, WithinAbs(92.56, 0.05));

    OrbitalElements geo{};
    geo.semiMajorAxis = 42164.0_km;
    REQUIRE_THAT(orbitalPeriod(geo, kGmEarth) / 3600.0, WithinAbs(23.934, 0.01));
}

TEST_CASE("the perifocal basis is orthonormal and oriented with the orbit",
          "[physics][elements]") {
    for (const auto& [name, elements] : testOrbits()) {
        INFO(name);
        const auto [p, q] = perifocalBasis(elements);

        // Unit length, mutually perpendicular.
        REQUIRE_THAT(p.norm(), WithinAbs(1.0, 1e-12));
        REQUIRE_THAT(q.norm(), WithinAbs(1.0, 1e-12));
        REQUIRE_THAT(dot(p, q), WithinAbs(0.0, 1e-12));

        // p x q is the orbit normal: the direction of the angular momentum
        // the same elements produce through stateFromElements.
        const StateVector state = stateFromElements(elements, kGmEarth, elements.epoch);
        const Vec3 h = cross(state.position, state.velocity).normalized();
        const Vec3 w = cross(p, q);
        REQUIRE_THAT(dot(h, w), WithinAbs(1.0, 1e-9));
    }
}

TEST_CASE("periapsis lies along p", "[physics][elements]") {
    // At M = 0 the body is at periapsis, so its position must point along p
    // with magnitude a(1 - e).
    for (const auto& [name, elements] : testOrbits()) {
        INFO(name);
        OrbitalElements atPeriapsis = elements;
        atPeriapsis.meanAnomalyAtEpoch = 0.0;

        const auto [p, q] = perifocalBasis(atPeriapsis);
        const StateVector state =
            stateFromElements(atPeriapsis, kGmEarth, atPeriapsis.epoch);

        const double rp = periapsisRadius(atPeriapsis);
        REQUIRE_THAT(dot(state.position, p), WithinRel(rp, 1e-9));
        REQUIRE_THAT(distance(state.position, p * rp), WithinAbs(0.0, 1e-3));
    }
}
