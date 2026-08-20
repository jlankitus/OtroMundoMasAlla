// Ascent guidance, validated by outcomes a flight dynamics engineer would
// check: does it reach the orbit it was aimed at, does it pay the delta-v
// budget real rockets pay (orbital speed PLUS gravity and drag losses — the
// losses are not modelled anywhere, they emerge from flying through the same
// physics as everything else), and does launching eastward genuinely cost
// less than launching westward, because the pad's rotation velocity is real.

#include "core/units.hpp"
#include "physics/orbital_elements.hpp"
#include "sim/world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace omma;

namespace {

constexpr double kGmEarth = 3.98600435507e14;

/// Step until the ascent program reports Done (or a step budget runs out —
/// a guidance bug must fail the test, not hang it).
bool flyToCompletion(World& world, SpacecraftId id, std::int64_t maxSteps) {
    for (std::int64_t i = 0; i < maxSteps; ++i) {
        world.step();
        const Spacecraft* craft = world.find(id);
        if (craft == nullptr || !craft->isAlive()) {
            return false;
        }
        if (craft->ascent.phase == AscentProgram::Phase::Done) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a pad launch flies itself to the orbit it was aimed at",
          "[sim][ascent]") {
    World world{SolarSystem::standard(), std::chrono::seconds{1}};

    SurfaceLaunchRequest request{};   // Canaveral, due east, 200 km
    const auto id = world.launchFromSurface(BodyId::Earth, request, "PATHFINDER");
    REQUIRE(id.isValid());

    // On the pad: essentially zero altitude, moving only with the ground.
    REQUIRE(world.altitudeOf(*world.find(id)) < 100.0);

    // A whole ascent plus coast and circularization fits well inside an hour.
    REQUIRE(flyToCompletion(world, id, 3'600));

    const Spacecraft& craft = *world.find(id);
    const auto elements = world.elementsOf(craft);

    // The closed loops aimed at 200 km; the open-loop pitch schedule decides
    // the rest, so the bounds are honest rather than tight.
    REQUIRE(periapsisRadius(elements) - 6.371e6 > 150.0e3);
    REQUIRE(apoapsisRadius(elements) - 6.371e6 < 300.0e3);
    REQUIRE(elements.eccentricity < 0.02);

    // Due-east from 28.5 N puts the orbit at the latitude's inclination.
    REQUIRE_THAT(toDegrees(elements.inclination), WithinAbs(28.5, 2.0));

    // The delta-v bill: ~7.8 km/s of orbital speed plus gravity and drag
    // losses, minus the rotation bonus. Real medium-lift vehicles pay about
    // 9-9.5 km/s; far outside this band means the physics lied somewhere.
    REQUIRE(craft.deltaVSpentMps > 8'000.0);
    REQUIRE(craft.deltaVSpentMps < 10'800.0);

    // And the engine is off.
    REQUIRE_FALSE(craft.thrust.active);
}

TEST_CASE("launching east is measurably cheaper than launching west",
          "[sim][ascent]") {
    // Same vehicle, same pad, opposite azimuths. The eastward run pockets the
    // pad's rotation velocity (~410 m/s at 28.5 N); the westward run pays to
    // cancel it and then some. The difference must show up in delta-v spent —
    // roughly twice the surface speed.
    auto fly = [](double azimuthRadians) {
        World world{SolarSystem::standard(), std::chrono::seconds{1}};
        SurfaceLaunchRequest request{};
        request.azimuthRadians = azimuthRadians;
        const auto id = world.launchFromSurface(BodyId::Earth, request, "AZ");
        REQUIRE(flyToCompletion(world, id, 3'600));
        return world.find(id)->deltaVSpentMps;
    };

    const double east = fly(toRadians(90.0));
    const double west = fly(toRadians(270.0));

    REQUIRE(west > east + 400.0);
    REQUIRE(west < east + 1'400.0);
}

TEST_CASE("a vehicle that cannot lift itself crashes instead of orbiting",
          "[sim][ascent]") {
    World world{SolarSystem::standard(), std::chrono::seconds{1}};

    SurfaceLaunchRequest request{};
    request.maxThrustNewtons = 50.0e3;   // T/W ~ 0.5: a lawn ornament
    const auto id = world.launchFromSurface(BodyId::Earth, request, "BRICK");
    REQUIRE(id.isValid());

    // It never completes, and it dies on (or into) the pad.
    REQUIRE_FALSE(flyToCompletion(world, id, 600));
    REQUIRE_FALSE(world.find(id)->isAlive());
}

TEST_CASE("ascent debug trace", "[.][ascentdebug]") {
    World world{SolarSystem::standard(), std::chrono::seconds{1}};
    SurfaceLaunchRequest request{};
    const auto id = world.launchFromSurface(BodyId::Earth, request, "DEBUG");

    for (int i = 0; i < 3000; ++i) {
        world.step();
        const Spacecraft* c = world.find(id);
        if (c == nullptr || !c->isAlive()) { WARN("died at t=" << i); break; }
        if (i % 20 == 0 || c->ascent.phase == AscentProgram::Phase::Done
            || c->ascent.phase == AscentProgram::Phase::Circularize) {
            const auto e = world.elementsOf(*c);
            WARN("t=" << i
                 << " phase=" << ascentPhaseName(c->ascent.phase)
                 << " alt=" << world.altitudeOf(*c) / 1000.0
                 << "km apo=" << (apoapsisRadius(e) - 6.371e6) / 1000.0
                 << "km peri=" << (periapsisRadius(e) - 6.371e6) / 1000.0
                 << "km prop=" << c->propellantKg
                 << "kg dv=" << c->deltaVSpentMps
                 << " e=" << e.eccentricity
                 << " setpoint=" << c->ascent.targetApoapsisRadius
                 << " throttle=" << c->thrust.throttle
                 << " active=" << c->thrust.active);
        }
        if (c->ascent.phase == AscentProgram::Phase::Done) break;
    }
    SUCCEED();
}
