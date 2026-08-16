// Drag, validated the way the other perturbations are: against numbers that
// can be computed independently. The secular decay rate of a circular orbit
// is da/orbit = -2 pi a^2 rho (Cd A / m) — comparing the measured decay to it
// checks the density model, the force direction and the bookkeeping at once.
//
// All apsis comparisons sample at whole-orbit multiples (same phase): J2's
// short-period wobble moves the osculating elements by kilometres within an
// orbit, and a mid-phase comparison measures the wobble, not the drag.

#include "core/units.hpp"
#include "physics/atmosphere.hpp"
#include "physics/orbital_elements.hpp"
#include "sim/world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using Catch::Matchers::WithinRel;
using namespace omma;

namespace {

LaunchRequest circularAt(double altitudeMetres) {
    LaunchRequest request{};
    request.name = "DRAGTEST";
    request.aroundBody = BodyId::Earth;
    request.altitudeMetres = altitudeMetres;
    request.inclinationRadians = toRadians(51.6);
    return request;
}

/// Osculating a, averaged over one full orbit. Averaging removes the J2
/// short-period oscillation entirely, where same-phase sampling only mostly
/// does — at 220 km, J2 shifts the osculating period enough that "one orbit
/// later" is several seconds off phase, which reads as a kilometre of fake
/// decay. The average is immune to the phase.
double orbitAveragedSemiMajor(World& world, SpacecraftId id) {
    const double gm = 3.98600435507e14;
    const auto period = static_cast<std::int64_t>(
        orbitalPeriod(world.elementsOf(*world.find(id)), gm));
    constexpr int kSamples = 64;

    double sum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        world.step(period / kSamples);
        sum += world.elementsOf(*world.find(id)).semiMajorAxis;
    }
    return sum / kSamples;
}

}  // namespace

TEST_CASE("the density model reproduces its own table", "[physics][drag]") {
    // Band bases evaluate to the tabulated density exactly.
    REQUIRE_THAT(earthAtmosphericDensity(400.0e3), WithinRel(3.725e-12, 1e-9));
    REQUIRE_THAT(earthAtmosphericDensity(100.0e3), WithinRel(5.297e-7, 1e-9));

    // Monotonically thinner with altitude, over the whole span an orbiter uses.
    double previous = earthAtmosphericDensity(90.0e3);
    for (double km = 100.0; km <= 1400.0; km += 25.0) {
        const double density = earthAtmosphericDensity(km * 1000.0);
        REQUIRE(density < previous);
        REQUIRE(density > 0.0);
        previous = density;
    }
}

TEST_CASE("a low orbit decays at the textbook rate", "[sim][drag]") {
    // 400 km, where the scale height (58 km) dwarfs the J2 altitude wobble
    // (~5 km) and the mean-vs-equatorial radius convention (~7 km). At 220 km
    // those effects sit in the exponent and inflate the density the sim
    // actually flies through by tens of percent — measured, not guessed: the
    // first version of this test ran there and read 1.67x the textbook rate,
    // consistently, at any sampling phase.
    World world{SolarSystem::standard(), std::chrono::seconds{1}};
    LaunchRequest request = circularAt(400.0e3);
    request.propellantKg = 80.0;    // mass pinned: B = CdA/m must match below
    const auto id = world.launch(request);
    REQUIRE(id.isValid());

    // Averages over orbit 1 and orbit 10: midpoints nine orbits apart.
    const double a0 = orbitAveragedSemiMajor(world, id);
    for (int i = 0; i < 8; ++i) {
        orbitAveragedSemiMajor(world, id);
    }
    const double a1 = orbitAveragedSemiMajor(world, id);
    const double decay = a0 - a1;

    constexpr int kOrbitsBetweenMidpoints = 9;
    const double density = earthAtmosphericDensity(400.0e3);
    const double ballistic = 2.2 * 1.5 / 280.0;
    const double expectedPerOrbit =
        constants::kTwoPi * a0 * a0 * density * ballistic;

    REQUIRE(decay > 0.0);                                    // it comes down
    REQUIRE_THAT(decay,
                 WithinRel(expectedPerOrbit * kOrbitsBetweenMidpoints, 0.30));
}

TEST_CASE("drag is negligible where the air is", "[sim][drag]") {
    World world{SolarSystem::standard(), std::chrono::seconds{1}};
    const auto id = world.launch(circularAt(1500.0e3));

    const double a0 = orbitAveragedSemiMajor(world, id);
    const double a1 = orbitAveragedSemiMajor(world, id);

    // Orbit-averaged residual from third-body is metres; drag at 1500 km is
    // millimetres per orbit. Anything beyond 30 m would be a defect.
    REQUIRE(std::abs(a1 - a0) < 30.0);
}

TEST_CASE("orbital energy only ever decreases under drag", "[sim][drag]") {
    World world{SolarSystem::standard(), std::chrono::seconds{1}};
    const auto id = world.launch(circularAt(250.0e3));
    const double gm = 3.98600435507e14;

    double previous = -gm / (2.0 * orbitAveragedSemiMajor(world, id));
    for (int orbit = 0; orbit < 3; ++orbit) {
        const double energy = -gm / (2.0 * orbitAveragedSemiMajor(world, id));
        REQUIRE(energy < previous);   // drag never adds energy
        previous = energy;
    }
}
