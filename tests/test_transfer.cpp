// The Hohmann planner, checked against the Earth-Mars numbers a mission
// design course drills until you know them by heart. Instantaneous radii make
// the bands generous — Mars' eccentricity (0.093) genuinely moves them —
// but a wrong formula misses these bands by multiples, not percent.

#include "core/units.hpp"
#include "sim/transfer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace omma;

namespace {
constexpr double kDay = 86'400.0;
constexpr double kParkingRadius = 6.771e6;   // 400 km LEO
}  // namespace

TEST_CASE("Earth to Mars looks like Earth to Mars", "[sim][transfer]") {
    const SolarSystem system = SolarSystem::standard();
    const auto plan = planHohmannTransfer(system, BodyId::Earth, BodyId::Mars,
                                          Epoch::j2000(), kParkingRadius);
    REQUIRE(plan.valid);

    // Time of flight: ~259 days for the mean orbits; Mars' eccentricity
    // stretches the band either way.
    REQUIRE(plan.transferSeconds / kDay > 220.0);
    REQUIRE(plan.transferSeconds / kDay < 300.0);

    // Departure asymptote and the injection burn out of LEO.
    REQUIRE_THAT(plan.vInfinityMps, Catch::Matchers::WithinAbs(2'940.0, 500.0));
    REQUIRE_THAT(plan.departureDeltaVMps,
                 Catch::Matchers::WithinAbs(3'600.0, 300.0));

    // The famous number is 44 degrees — FOR MEAN RADII. At J2000 Mars sits
    // near perihelion (1.39 au, not 1.52), the half-ellipse is shorter, and
    // the required lead is honestly larger: the folklore value is one epoch's
    // special case of this band, not the truth the planner must repeat.
    REQUIRE(plan.phaseAngleDeg > 38.0);
    REQUIRE(plan.phaseAngleDeg < 62.0);

    // The window arrives within one synodic period (~780 days).
    REQUIRE(plan.waitSeconds >= 0.0);
    REQUIRE(plan.waitSeconds / kDay < 800.0);
}

TEST_CASE("inbound windows exist too", "[sim][transfer]") {
    const SolarSystem system = SolarSystem::standard();
    const auto plan = planHohmannTransfer(system, BodyId::Mars, BodyId::Earth,
                                          Epoch::j2000(), 3.7e6);
    REQUIRE(plan.valid);
    REQUIRE(plan.waitSeconds >= 0.0);
    REQUIRE(plan.waitSeconds / kDay < 800.0);
    // Inbound requires the target to LAG: a negative lead angle.
    REQUIRE(plan.phaseAngleDeg < 0.0);
}

TEST_CASE("degenerate transfers are refused, not computed", "[sim][transfer]") {
    const SolarSystem system = SolarSystem::standard();
    REQUIRE_FALSE(planHohmannTransfer(system, BodyId::Earth, BodyId::Earth,
                                      Epoch::j2000(), kParkingRadius).valid);
    REQUIRE_FALSE(planHohmannTransfer(system, BodyId::Earth, BodyId::Sun,
                                      Epoch::j2000(), kParkingRadius).valid);
}

TEST_CASE("the window is when the phase is right", "[sim][transfer]") {
    // Advance the clock TO the window the planner names, re-plan, and the
    // current phase must now match the required phase - the planner's own
    // prediction validates itself.
    const SolarSystem system = SolarSystem::standard();
    const auto first = planHohmannTransfer(system, BodyId::Earth, BodyId::Mars,
                                           Epoch::j2000(), kParkingRadius);
    REQUIRE(first.valid);

    const Epoch atWindow = Epoch::j2000() + fromSeconds(first.waitSeconds);
    const auto then = planHohmannTransfer(system, BodyId::Earth, BodyId::Mars,
                                          atWindow, kParkingRadius);
    // Mean-motion linearization leaves the short-period true-anomaly wiggle
    // unmodelled: up to ~2e radians, which for Mars (e = 0.093) is ~11 deg.
    // Real departure windows are weeks wide; this planner names the week.
    REQUIRE_THAT(then.currentPhaseAngleDeg,
                 Catch::Matchers::WithinAbs(then.phaseAngleDeg, 15.0));
}
