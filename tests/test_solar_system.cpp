// Validation of the solar system model against published astronomy.
//
// Every number asserted here is one you can look up independently. That is the
// point: a physics model is only as trustworthy as its agreement with reality,
// and "the code returns what the code computes" is not a test. If someone
// fat-fingers a digit in the JPL table, one of these fails.

#include "core/units.hpp"
#include "physics/solar_system.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::BodyId;
using omma::Epoch;
using omma::SolarSystem;
using omma::constants::kAu;
using omma::constants::kSecondsPerDay;

namespace {

const SolarSystem& system() {
    static const SolarSystem s = SolarSystem::standard();
    return s;
}

double distanceFromSunAu(BodyId id, Epoch t) {
    return system()[id].sample(t).position.norm() / kAu;
}

}  // namespace

TEST_CASE("the Sun sits at the origin and does not move", "[physics][solar]") {
    const auto& sun = system()[BodyId::Sun];
    for (const Epoch t : {Epoch::j2000(),
                          Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 0, 0, 0.0}),
                          Epoch::fromCivil(omma::CivilTime{1950, 1, 1, 0, 0, 0.0})}) {
        REQUIRE(sun.sample(t).position == omma::Vec3::zero());
        REQUIRE(sun.sample(t).velocity == omma::Vec3::zero());
    }
    REQUIRE(sun.name() == "Sun");
}

TEST_CASE("orbital periods match published values", "[physics][solar]") {
    // Sidereal periods in days, from the JPL planetary fact sheets.
    struct Expected { BodyId id; double days; };
    const Expected expected[] = {
        {BodyId::Mercury,    87.969},
        {BodyId::Venus,     224.701},
        {BodyId::Earth,     365.256},
        {BodyId::Mars,      686.980},
        {BodyId::Jupiter,  4332.589},
        {BodyId::Saturn,  10759.22},
        {BodyId::Uranus,  30685.4},
        {BodyId::Neptune, 60189.0},
        {BodyId::Pluto,   90560.0},
    };

    for (const auto& [id, days] : expected) {
        const auto* body = dynamic_cast<const omma::KeplerEphemeris*>(&system()[id]);
        REQUIRE(body != nullptr);
        const double computed = body->orbitalPeriodSeconds(Epoch::j2000()) / kSecondsPerDay;
        INFO(omma::bodyName(id) << ": computed " << computed << " d, expected " << days);
        REQUIRE_THAT(computed, WithinRel(days, 0.005));   // 0.5%
    }
}

TEST_CASE("the Moon's period is one sidereal month", "[physics][solar]") {
    const auto* moon = dynamic_cast<const omma::KeplerEphemeris*>(&system()[BodyId::Moon]);
    REQUIRE(moon != nullptr);

    // 27.321661 days. Getting this right requires using GM_earth + GM_moon
    // rather than GM_earth alone -- the Moon is 1/81 of Earth's mass, so the
    // pair genuinely orbits a shared barycenter and the correction is 1.2%,
    // worth about four hours of period.
    REQUIRE_THAT(moon->orbitalPeriodSeconds(Epoch::j2000()) / kSecondsPerDay,
                 WithinRel(27.321661, 0.001));
}

TEST_CASE("planets sit at their known distances from the Sun", "[physics][solar]") {
    // Every planet must stay between its perihelion and aphelion, checked
    // across two centuries so the secular element rates get exercised too.
    struct Range { BodyId id; double perihelionAu; double aphelionAu; };
    const Range ranges[] = {
        {BodyId::Mercury,  0.307,  0.467},
        {BodyId::Venus,    0.718,  0.729},
        {BodyId::Earth,    0.983,  1.017},
        {BodyId::Mars,     1.381,  1.666},
        {BodyId::Jupiter,  4.950,  5.459},
        {BodyId::Saturn,   9.041, 10.124},
        {BodyId::Uranus,  18.286, 20.096},
        {BodyId::Neptune, 29.810, 30.330},
        {BodyId::Pluto,   29.658, 49.305},
    };

    for (const auto& [id, perihelion, aphelion] : ranges) {
        double minR = 1e9;
        double maxR = 0.0;
        // Sample every 10 days across the model's full validity window,
        // 1800-2050. That window is 250 years, chosen because it is the
        // shortest span that contains a complete revolution of every body
        // here -- Pluto's 248-year orbit is the binding constraint, and a
        // 150-year window silently never reaches its aphelion at all. A test
        // that samples less than one period does not test the orbit; it tests
        // the arc you happened to look at.
        constexpr int kDaysBeforeJ2000 = 73'048;   // to 1800-01-01
        for (int day = 0; day < 91'300; day += 10) {
            const Epoch t = Epoch::j2000() + std::chrono::hours{24 * (day - kDaysBeforeJ2000)};
            const double r = distanceFromSunAu(id, t);
            minR = std::min(minR, r);
            maxR = std::max(maxR, r);
        }
        INFO(omma::bodyName(id) << ": sampled " << minR << " .. " << maxR
                                << " au, expected " << perihelion << " .. " << aphelion);
        REQUIRE_THAT(minR, WithinRel(perihelion, 0.01));
        REQUIRE_THAT(maxR, WithinRel(aphelion, 0.01));
    }
}

TEST_CASE("Earth is at perihelion in early January", "[physics][solar]") {
    // A fact anyone can check against an almanac, and one that depends on the
    // longitude of perihelion being transcribed correctly -- an element that a
    // distance-range test would not catch if it were wrong.
    double closestDay = 0.0;
    double closest = 1e9;
    const Epoch yearStart = Epoch::fromCivil(omma::CivilTime{2026, 1, 1, 0, 0, 0.0});

    for (int day = 0; day < 365; ++day) {
        const Epoch t = yearStart + std::chrono::hours{24 * day};
        const double r = distanceFromSunAu(BodyId::Earth, t);
        if (r < closest) {
            closest = r;
            closestDay = static_cast<double>(day);
        }
    }

    INFO("perihelion found on day " << closestDay << " of 2026 at " << closest << " au");
    REQUIRE(closestDay < 10.0);                        // first week or so of January
    REQUIRE_THAT(closest, WithinAbs(0.9833, 0.001));
}

TEST_CASE("Earth's orbital speed varies the way Kepler said it would",
          "[physics][solar]") {
    // Earth's MEAN orbital speed is 29.78 km/s -- but it is only travelling at
    // the mean twice a year. J2000 is 1 January, two days before perihelion,
    // where Earth is moving fastest at 30.29 km/s. Six months later, at
    // aphelion, it is down to 29.29 km/s.
    //
    // This test originally asserted 29.78 at J2000 and failed. The model was
    // right and the expectation was wrong: an instantaneous speed had been
    // compared against an annual average. Worth leaving as a comment, because
    // "the test failed so the code is broken" is wrong often enough to be a
    // habit worth breaking.
    const auto& earth = system()[BodyId::Earth];

    const double atJ2000 = earth.sample(Epoch::j2000()).velocity.norm() / 1000.0;
    REQUIRE_THAT(atJ2000, WithinAbs(30.29, 0.05));

    double fastest = 0.0;
    double slowest = 1e9;
    double sum = 0.0;
    constexpr int kSamples = 3653;                     // ten years of daily samples
    for (int day = 0; day < kSamples; ++day) {
        const double v =
            earth.sample(Epoch::j2000() + std::chrono::hours{24 * day}).velocity.norm() / 1000.0;
        fastest = std::max(fastest, v);
        slowest = std::min(slowest, v);
        sum += v;
    }

    REQUIRE_THAT(fastest, WithinAbs(30.29, 0.05));     // perihelion, early January
    REQUIRE_THAT(slowest, WithinAbs(29.29, 0.05));     // aphelion, early July
    REQUIRE_THAT(sum / kSamples, WithinAbs(29.78, 0.05));   // and NOW the textbook mean
}

TEST_CASE("the Moon stays within its known distance range of Earth",
          "[physics][solar]") {
    // Perigee 356,500 km, apogee 406,700 km. The Kepler model cannot reproduce
    // the Sun-driven perturbations (evection, variation), so the tolerance is
    // deliberately loose -- a few thousand kilometres. Asserting tighter than
    // the model can deliver would produce a test that fails for being honest.
    const auto& earth = system()[BodyId::Earth];
    const auto& moon = system()[BodyId::Moon];

    double minKm = 1e9;
    double maxKm = 0.0;
    for (int hour = 0; hour < 24 * 400; hour += 6) {
        const Epoch t = Epoch::j2000() + std::chrono::hours{hour};
        const double d = distance(moon.sample(t).position, earth.sample(t).position) / 1000.0;
        minKm = std::min(minKm, d);
        maxKm = std::max(maxKm, d);
    }

    INFO("Moon ranged " << minKm << " .. " << maxKm << " km from Earth");
    REQUIRE_THAT(minKm, WithinAbs(363'300.0, 5'000.0));
    REQUIRE_THAT(maxKm, WithinAbs(405'500.0, 5'000.0));
}

TEST_CASE("the Moon's absolute position is Earth's plus its own orbit",
          "[physics][solar]") {
    // sample() composes up the parent chain. Verifying it explicitly matters
    // because getting this backwards produces a Moon that orbits the Sun on
    // its own, which looks almost right at solar-system zoom.
    const auto* moon = dynamic_cast<const omma::KeplerEphemeris*>(&system()[BodyId::Moon]);
    REQUIRE(moon != nullptr);
    REQUIRE(moon->parent() == &system()[BodyId::Earth]);

    const Epoch t = Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 0, 0, 0.0});
    const auto absolute = moon->sample(t);
    const auto relative = moon->sampleRelativeToParent(t);
    const auto earth = system()[BodyId::Earth].sample(t);

    REQUIRE_THAT(distance(absolute.position, earth.position + relative.position),
                 WithinAbs(0.0, 1e-6));

    // And the Moon is roughly a light-second away from Earth, not an au.
    REQUIRE_THAT(relative.position.norm() / 1000.0, WithinAbs(384'000.0, 25'000.0));
}

TEST_CASE("planetary orbits lie close to the ecliptic", "[physics][solar]") {
    // By definition of the frame, Earth's inclination is ~0 and everything
    // except Pluto and Mercury is within a few degrees. A sign error in the
    // rotation matrix would throw a planet well out of plane.
    struct Expected { BodyId id; double inclinationDeg; };
    const Expected expected[] = {
        {BodyId::Mercury,  7.005}, {BodyId::Venus,   3.395},
        {BodyId::Earth,    0.000}, {BodyId::Mars,    1.850},
        {BodyId::Jupiter,  1.304}, {BodyId::Saturn,  2.486},
        {BodyId::Uranus,   0.773}, {BodyId::Neptune, 1.770},
        {BodyId::Pluto,   17.140},
    };

    for (const auto& [id, inclinationDeg] : expected) {
        const auto& body = system()[id];
        const auto state = body.sample(Epoch::j2000());
        const auto h = cross(state.position, state.velocity);
        const double tilt = omma::toDegrees(
            omma::angleBetween(h, omma::Vec3::unitZ()));
        INFO(omma::bodyName(id) << ": plane tilt " << tilt << " deg");
        REQUIRE_THAT(tilt, WithinAbs(inclinationDeg, 0.01));
    }
}

TEST_CASE("sampling is pure: same epoch, same answer", "[physics][solar][determinism]") {
    // An ephemeris must be a function of time and nothing else. Any hidden
    // state -- a cache keyed on the last query, a lazily-initialised member --
    // would break replay in a way that is agonising to track down.
    const Epoch t = Epoch::fromCivil(omma::CivilTime{2033, 3, 14, 15, 9, 26.5});

    for (const auto& body : system().bodies()) {
        const auto first = body->sample(t);
        // Interleave other queries to disturb any hypothetical cache.
        for (int i = 0; i < 10; ++i) {
            static_cast<void>(body->sample(t + std::chrono::hours{i * 137}));
        }
        const auto again = body->sample(t);
        INFO(body->name());
        REQUIRE(first == again);      // bit-identical, not merely close
    }
}

TEST_CASE("any epoch is reachable in one call", "[physics][solar]") {
    // The property that makes unbounded time warp possible: jumping a century
    // ahead costs exactly as much as advancing one second. No integration, no
    // stepping, no accumulated error.
    const Epoch far = Epoch::fromCivil(omma::CivilTime{2049, 12, 31, 0, 0, 0.0});
    const Epoch past = Epoch::fromCivil(omma::CivilTime{1801, 1, 1, 0, 0, 0.0});

    for (const auto& body : system().bodies()) {
        INFO(body->name());
        REQUIRE(body->sample(far).isFinite());
        REQUIRE(body->sample(past).isFinite());
    }

    // Neptune has not completed an orbit since its discovery in 1846; over
    // 248 years it should move a long way, but stay at its own distance.
    const double thenAu = distanceFromSunAu(BodyId::Neptune, past);
    const double laterAu = distanceFromSunAu(BodyId::Neptune, far);
    REQUIRE_THAT(thenAu, WithinAbs(30.0, 0.6));
    REQUIRE_THAT(laterAu, WithinAbs(30.0, 0.6));
}

TEST_CASE("gravitational parameters and radii are sane", "[physics][solar]") {
    // Ordering checks rather than value checks: they catch a swapped pair of
    // table rows, which value tolerances wide enough to be maintainable will
    // happily let through.
    const auto gm = [](BodyId id) { return system()[id].gravitationalParameter(); };
    const auto radius = [](BodyId id) { return system()[id].meanRadius(); };

    REQUIRE(gm(BodyId::Sun) > gm(BodyId::Jupiter) * 1000.0);
    REQUIRE(gm(BodyId::Jupiter) > gm(BodyId::Saturn));
    REQUIRE(gm(BodyId::Saturn) > gm(BodyId::Neptune));
    REQUIRE(gm(BodyId::Neptune) > gm(BodyId::Uranus));   // Neptune is denser
    REQUIRE(gm(BodyId::Earth) > gm(BodyId::Venus));
    REQUIRE(gm(BodyId::Earth) > gm(BodyId::Moon) * 80.0);
    REQUIRE(gm(BodyId::Moon) > gm(BodyId::Pluto));

    REQUIRE(radius(BodyId::Sun) > radius(BodyId::Jupiter) * 9.0);
    REQUIRE(radius(BodyId::Jupiter) > radius(BodyId::Saturn));
    REQUIRE(radius(BodyId::Uranus) > radius(BodyId::Neptune));   // Uranus is larger
    REQUIRE(radius(BodyId::Earth) > radius(BodyId::Venus));

    // Earth's surface gravity, from GM/r^2. Should be 9.8 m/s^2.
    REQUIRE_THAT(gm(BodyId::Earth) / (radius(BodyId::Earth) * radius(BodyId::Earth)),
                 WithinAbs(9.82, 0.05));
    // And the Moon's should be about a sixth of that.
    REQUIRE_THAT(gm(BodyId::Moon) / (radius(BodyId::Moon) * radius(BodyId::Moon)),
                 WithinAbs(1.62, 0.05));
}

TEST_CASE("bodies can be looked up by name", "[physics][solar]") {
    REQUIRE(system().findByName("Mars") == &system()[BodyId::Mars]);
    REQUIRE(system().findByName("Moon") == &system()[BodyId::Moon]);
    REQUIRE(system().findByName("Vulcan") == nullptr);
    REQUIRE(omma::bodyName(BodyId::Saturn) == "Saturn");
    REQUIRE(system().size() == static_cast<std::size_t>(BodyId::Count));
}
