// World and Spacecraft: launching, burning, and the consequences.
//
// The interesting tests here are the ones that assert orbital mechanics doing
// something counter-intuitive, because those are the cases where a plausible
// implementation is wrong and a plausible expectation is also wrong.

#include "core/units.hpp"
#include "sim/world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace omma::literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::BodyId;
using omma::Epoch;
using omma::LaunchRequest;
using omma::SimEvent;
using omma::ThrustCommand;
using omma::World;

namespace {

World makeWorld(omma::Duration step = 1s) {
    return World{omma::SolarSystem::standard(), step,
                 Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 0, 0, 0.0})};
}

LaunchRequest leo(double altitude = 400.0_km) {
    LaunchRequest request{};
    request.name = "TESTSAT";
    request.aroundBody = BodyId::Earth;
    request.altitudeMetres = altitude;
    return request;
}

/// A craft with enough thrust that a burn is effectively impulsive.
///
/// This distinction mattered more than expected. The default 22 N thruster on a
/// 250 kg craft gives 0.088 m/s^2, so 100 m/s of delta-v takes about 1100
/// SECONDS -- a fifth of a low-orbit period. That is realistic for a
/// monopropellant thruster, and it is NOT an impulsive burn: the thrust direction
/// rotates with the craft as it goes, periapsis moves, and apoapsis rises well
/// short of the textbook value. Comparing that against the impulsive prediction
/// is comparing against the wrong model.
///
/// Two different behaviours, so two fixtures. This one burns in seconds and can
/// be checked against closed-form two-body results.
LaunchRequest impulsiveLeo(double altitude = 400.0_km) {
    LaunchRequest request = leo(altitude);
    request.maxThrustNewtons = 4000.0;    // ~16 m/s^2: 100 m/s in about 6 s
    request.propellantKg = 120.0;
    return request;
}

/// Circular orbital speed at a given radius about Earth.
double circularSpeed(double radius) {
    return std::sqrt(3.98600435507e14 / radius);
}

bool hasEvent(const std::vector<SimEvent>& events, SimEvent::Kind kind) {
    return std::any_of(events.begin(), events.end(),
                       [kind](const SimEvent& e) { return e.kind == kind; });
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Launching
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a launched spacecraft is where it was asked to be", "[sim][world]") {
    World world = makeWorld();
    const auto id = world.launch(leo(400.0_km));
    REQUIRE(id.isValid());

    const auto* craft = world.find(id);
    REQUIRE(craft != nullptr);
    REQUIRE_THAT(world.altitudeOf(*craft), WithinAbs(400.0_km, 1.0));
    REQUIRE(hasEvent(world.peekEvents(), SimEvent::Kind::Launched));

    SECTION("and its orbit is the requested one") {
        const auto elements = world.elementsOf(*craft);
        REQUIRE_THAT(elements.eccentricity, WithinAbs(0.0, 1e-9));
        // 6371 + 400 km, so a period of about 92.4 minutes.
        const double gm = world.system()[BodyId::Earth].gravitationalParameter();
        REQUIRE_THAT(orbitalPeriod(elements, gm) / 60.0, WithinAbs(92.4, 0.2));
    }
}

TEST_CASE("impossible orbits are refused rather than accepted quietly",
          "[sim][world]") {
    World world = makeWorld();

    LaunchRequest underground = leo();
    underground.altitudeMetres = -100.0_km;
    REQUIRE_FALSE(world.launch(underground).isValid());

    LaunchRequest escaping = leo();
    escaping.eccentricity = 1.2;
    REQUIRE_FALSE(world.launch(escaping).isValid());

    // A refused launch must not have created anything.
    REQUIRE(world.spacecraft().empty());
}

TEST_CASE("a launched craft stays in orbit for days", "[sim][world]") {
    World world = makeWorld(5s);
    const auto id = world.launch(leo(550.0_km));

    world.step(3 * 86'400 / 5);      // three days

    const auto* craft = world.find(id);
    REQUIRE(craft != nullptr);
    REQUIRE(craft->isAlive());
    const double altitude = world.altitudeOf(*craft);
    INFO("altitude after three days: " << altitude / 1000.0 << " km");
    // Third-body perturbation from the Sun and Moon is tens of km over three
    // days. Anything wilder means the frame or the field is wrong.
    REQUIRE(altitude > 500.0_km);
    REQUIRE(altitude < 600.0_km);
}

TEST_CASE("a spacecraft in the wrong place hits the planet", "[sim][world]") {
    // A very eccentric orbit whose periapsis is inside the atmosphere is not
    // survivable, and the sim should say so rather than letting it tunnel through.
    World world = makeWorld(1s);
    LaunchRequest suborbital = leo(20.0_km);   // 20 km "altitude": inside the air
    suborbital.eccentricity = 0.6;
    const auto id = world.launch(suborbital);
    REQUIRE(id.isValid());

    // Start at apoapsis so it has to fall back down.
    world.step(6000);

    const auto* craft = world.find(id);
    REQUIRE(craft != nullptr);
    INFO("mode after 6000 s, altitude " << world.altitudeOf(*craft) / 1000.0 << " km");
    // Either it has hit, or it is still above the surface — never below it and
    // still flying.
    if (craft->isAlive()) {
        REQUIRE(world.altitudeOf(*craft) > -1.0);
    } else {
        REQUIRE(hasEvent(world.peekEvents(), SimEvent::Kind::Collided));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The tick contract
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("stepping n times equals one step of n", "[sim][world][determinism]") {
    // A replay driven by recorded step counts depends on this exactly.
    const auto run = [](bool batched) {
        World world = makeWorld(1s);
        const auto id = world.launch(leo(500.0_km));
        if (batched) {
            world.step(4000);
        } else {
            for (int i = 0; i < 4000; ++i) {
                world.step();
            }
        }
        return world.find(id)->state;
    };

    REQUIRE(run(false) == run(true));   // bit-identical, not merely close
}

TEST_CASE("the whole world is bit-reproducible", "[sim][world][determinism]") {
    const auto run = [] {
        World world = makeWorld(2s);
        const auto a = world.launch(leo(400.0_km));
        LaunchRequest polar = leo(700.0_km);
        polar.name = "POLAR";
        polar.inclinationRadians = 98.0_deg;
        const auto b = world.launch(polar);

        world.step(500);
        world.commandDeltaV(a, ThrustCommand::Frame::Prograde, 40.0);
        world.step(2000);
        world.commandDeltaV(b, ThrustCommand::Frame::Normal, 15.0);
        world.step(2000);

        return std::pair{world.find(a)->state, world.find(b)->state};
    };

    const auto first = run();
    const auto second = run();
    REQUIRE(first.first == second.first);
    REQUIRE(first.second == second.second);
}

// ─────────────────────────────────────────────────────────────────────────────
// Burns — where orbital mechanics stops being intuitive
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a prograde burn raises the OPPOSITE side of the orbit",
          "[sim][world][burn]") {
    // The single most counter-intuitive fact in orbital mechanics. Thrusting
    // forward does not move you forward; it raises your altitude half an orbit
    // later, and leaves the point you burned at almost exactly where it was.
    // Asserted against the CLOSED-FORM two-body answer rather than a hand-picked
    // margin, so this checks the physics and not merely the sign:
    //
    //     v1 = sqrt(GM/r) + dv,  E = v1^2/2 - GM/r,  a = -GM/2E
    //     apoapsis = 2a - r        (periapsis stays at the burn point)
    constexpr double kGm = 3.98600435507e14;
    constexpr double kDeltaV = 100.0;

    World world = makeWorld(1s);
    const auto id = world.launch(impulsiveLeo(400.0_km));

    const auto before = world.elementsOf(*world.find(id));
    const double r = periapsisRadius(before);

    const double v1 = circularSpeed(r) + kDeltaV;
    const double energy = 0.5 * v1 * v1 - kGm / r;
    const double predictedApoapsis = 2.0 * (-kGm / (2.0 * energy)) - r;

    REQUIRE(world.commandDeltaV(id, ThrustCommand::Frame::Prograde, kDeltaV));
    world.step(120);                  // a 6 s burn finishes far inside this

    const auto after = world.elementsOf(*world.find(id));
    INFO("apoapsis " << apoapsisRadius(before) / 1000.0 << " -> "
         << apoapsisRadius(after) / 1000.0 << " km  (predicted "
         << predictedApoapsis / 1000.0 << ")");

    REQUIRE_THAT(apoapsisRadius(after), WithinRel(predictedApoapsis, 0.02));
    // Periapsis barely moves: that is what "the opposite side" means.
    REQUIRE_THAT(periapsisRadius(after), WithinRel(r, 0.005));
}

TEST_CASE("a low-thrust burn is NOT impulsive, and that is realistic",
          "[sim][world][burn]") {
    // The companion to the test above, and the reason it needed its own fixture.
    //
    // The stock 22 N thruster needs about 1100 s to deliver 100 m/s. Spread over a
    // fifth of the orbit, the thrust direction rotates with the craft, so apoapsis
    // rises considerably less than the closed-form value and periapsis moves too.
    //
    // Recorded as a test rather than a comment because the impulsive test above
    // first failed for exactly this reason, and the model was right both times.
    constexpr double kGm = 3.98600435507e14;
    constexpr double kDeltaV = 100.0;

    World world = makeWorld(1s);
    const auto id = world.launch(leo(400.0_km));      // stock 22 N thruster

    const double r = periapsisRadius(world.elementsOf(*world.find(id)));
    const double v1 = circularSpeed(r) + kDeltaV;
    const double impulsiveApoapsis =
        2.0 * (-kGm / (2.0 * (0.5 * v1 * v1 - kGm / r))) - r;

    REQUIRE(world.commandDeltaV(id, ThrustCommand::Frame::Prograde, kDeltaV));
    world.step(1400);                 // long enough for the whole slow burn

    const auto after = world.elementsOf(*world.find(id));
    INFO("finite burn reached apoapsis " << apoapsisRadius(after) / 1000.0
         << " km; an impulse would have reached " << impulsiveApoapsis / 1000.0);

    REQUIRE(apoapsisRadius(after) > r);                       // it did go up
    REQUIRE(apoapsisRadius(after) < impulsiveApoapsis);       // but by less
    REQUIRE_FALSE(world.find(id)->thrust.active);             // and it finished
}

TEST_CASE("a retrograde burn lowers the opposite side", "[sim][world][burn]") {
    World world = makeWorld(1s);
    const auto id = world.launch(impulsiveLeo(800.0_km));
    const auto before = world.elementsOf(*world.find(id));

    REQUIRE(world.commandDeltaV(id, ThrustCommand::Frame::Retrograde, 80.0));
    world.step(120);

    const auto after = world.elementsOf(*world.find(id));
    // The claim is that the OPPOSITE side falls — here by hundreds of km —
    // while the burn point barely moves. Assert that, with real margins:
    // J2's short-period wobble shifts the osculating apsides by tens of
    // metres, and a hair's-width inequality on apoapsis measured the wobble
    // rather than the burn.
    REQUIRE(periapsisRadius(after) < periapsisRadius(before) - 100.0_km);
    REQUIRE_THAT(apoapsisRadius(after), WithinRel(apoapsisRadius(before), 1e-3));
}

TEST_CASE("a normal burn changes inclination and little else",
          "[sim][world][burn]") {
    // Burning along the angular momentum vector tilts the orbital plane without
    // changing its size. This is the only way to change inclination, and it is
    // brutally expensive: a full 90-degree plane change costs more delta-v than
    // reaching orbit did.
    constexpr double kDeltaV = 120.0;

    // For an impulse perpendicular to the velocity the plane tips by atan(dv / v).
    // At 7.56 km/s, 120 m/s buys about 0.91 degrees -- which is why a full
    // 90-degree plane change costs more delta-v than reaching orbit did.
    //
    // Checked at two very different thrust levels, and this is why.
    //
    // The first version of this test measured 0.97 degrees against a predicted
    // 0.91, and the explanation offered was a physics subtlety: Frame::Normal
    // tracks cross(r, v) of the instantaneous state, so a slow burn should
    // over-rotate. Plausible, and wrong. Testing the hypothesis -- more thrust
    // should converge on the impulsive answer -- produced 4.72 degrees instead,
    // five times the prediction, which is not a subtlety but a defect.
    //
    // It was two defects in World, both found by refusing to widen the tolerance:
    //
    //   * a burn cutoff was not honoured within a step, so a 0.19 s burn got a
    //     full second of thrust (5.2x), and every burn over-delivered by the
    //     length of its final partial step (the original 7%)
    //   * mass was held at its start-of-step value, so a step consuming 5% of the
    //     craft under-delivered by dm/m0 versus ve*ln(m0/m1) -- 2.7% low
    //
    // Two thrust levels stay in the test permanently, because a single one cannot
    // distinguish "the physics is subtle" from "the integration is wrong".
    const auto tiltFor = [](double thrustNewtons) {
        World world = makeWorld(1s);
        LaunchRequest request = impulsiveLeo(600.0_km);
        request.maxThrustNewtons = thrustNewtons;
        const auto id = world.launch(request);

        const auto before = world.elementsOf(*world.find(id));
        REQUIRE(world.commandDeltaV(id, ThrustCommand::Frame::Normal, kDeltaV));
        world.step(200);
        const auto after = world.elementsOf(*world.find(id));

        REQUIRE_THAT(after.semiMajorAxis, WithinRel(before.semiMajorAxis, 0.01));
        REQUIRE(before.inclination < 1e-9);
        return after.inclination;
    };

    const double predictedTilt = std::atan(kDeltaV / circularSpeed(6971.0_km));

    const double slow = tiltFor(4'000.0);        // ~9 s burn
    const double fast = tiltFor(200'000.0);      // ~0.2 s burn, near-impulsive

    INFO("predicted impulse " << omma::toDegrees(predictedTilt) << " deg;  "
         << "9 s burn " << omma::toDegrees(slow) << ";  "
         << "0.2 s burn " << omma::toDegrees(fast));

    // Both must match the closed-form impulse closely, at thrust levels three
    // orders of magnitude apart. Either bug above breaks one of these.
    REQUIRE_THAT(slow, WithinRel(predictedTilt, 0.01));
    REQUIRE_THAT(fast, WithinRel(predictedTilt, 0.01));
}

TEST_CASE("prograde is relative to the CENTRAL BODY, not the Sun",
          "[sim][world][burn]") {
    // Earth moves at 30 km/s around the Sun; a satellite moves at 7.7 km/s around
    // Earth. If "prograde" were resolved against the heliocentric velocity, every
    // burn would point roughly along Earth's orbital motion instead of along the
    // satellite's, and the satellite's orbit would change in a way that has
    // nothing to do with what was commanded.
    //
    // Detected by burning prograde and checking the orbit around EARTH grew.
    World world = makeWorld(1s);
    const auto id = world.launch(impulsiveLeo(400.0_km));

    const double energyBefore =
        specificOrbitalEnergy(world.relativeState(*world.find(id)),
                              world.system()[BodyId::Earth].gravitationalParameter());

    REQUIRE(world.commandDeltaV(id, ThrustCommand::Frame::Prograde, 60.0));
    world.step(120);

    const double energyAfter =
        specificOrbitalEnergy(world.relativeState(*world.find(id)),
                              world.system()[BodyId::Earth].gravitationalParameter());

    // Energy relative to Earth must INCREASE. A heliocentric-frame mistake would
    // move it in an essentially random direction.
    INFO("specific energy about Earth: " << energyBefore << " -> " << energyAfter);
    REQUIRE(energyAfter > energyBefore);
}

// ─────────────────────────────────────────────────────────────────────────────
// Propulsion accounting
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a burn spends propellant and delivers the delta-v it promised",
          "[sim][world][burn]") {
    World world = makeWorld(1s);
    const auto id = world.launch(leo(500.0_km));

    const double propellantBefore = world.find(id)->propellantKg;
    constexpr double kRequested = 75.0;

    REQUIRE(world.commandDeltaV(id, ThrustCommand::Frame::Prograde, kRequested));
    world.step(900);

    const auto* craft = world.find(id);
    REQUIRE_FALSE(craft->thrust.active);          // finished on its own
    REQUIRE(craft->propellantKg < propellantBefore);

    INFO("requested " << kRequested << " m/s, spent " << craft->deltaVSpentMps
         << " m/s, propellant " << propellantBefore << " -> " << craft->propellantKg);
    // Within a percent. The accounting integrates acceleration over a discrete
    // step while commandDeltaV inverts the rocket equation in closed form, so
    // exact agreement would be suspicious rather than reassuring.
    REQUIRE_THAT(craft->deltaVSpentMps, WithinRel(kRequested, 0.02));
}

TEST_CASE("the rocket equation is respected", "[sim][world][burn]") {
    World world = makeWorld();
    LaunchRequest request = leo();
    request.dryMassKg = 200.0;
    request.propellantKg = 50.0;
    request.exhaustVelocity = 2200.0;
    const auto id = world.launch(request);

    // dv = ve * ln(m0/m1) = 2200 * ln(250/200) = 491 m/s.
    REQUIRE_THAT(world.find(id)->remainingDeltaVMps(), WithinRel(491.0, 0.01));

    SECTION("and a burn you cannot afford is refused, not half-performed") {
        REQUIRE_FALSE(world.commandDeltaV(id, ThrustCommand::Frame::Prograde, 5000.0));
        REQUIRE_FALSE(world.find(id)->thrust.active);
        REQUIRE(world.find(id)->propellantKg == 50.0);
    }
}

TEST_CASE("a craft gets lighter as it burns, so it accelerates harder",
          "[sim][world][burn]") {
    // The reason the rocket equation has a logarithm in it. Constant thrust on a
    // shrinking mass is rising acceleration.
    World world = makeWorld(1s);
    LaunchRequest request = leo();
    request.dryMassKg = 50.0;
    request.propellantKg = 100.0;    // deliberately propellant-heavy
    request.maxThrustNewtons = 400.0;
    const auto id = world.launch(request);

    const double accelerationBefore = world.find(id)->maxAccelerationMps2();
    REQUIRE(world.commandBurn(id, ThrustCommand::Frame::Prograde, 1.0, 60s));
    world.step(60);
    const double accelerationAfter = world.find(id)->maxAccelerationMps2();

    INFO(accelerationBefore << " -> " << accelerationAfter << " m/s^2");
    REQUIRE(accelerationAfter > accelerationBefore * 1.05);
}

TEST_CASE("running out of propellant ends the burn and says so",
          "[sim][world][burn]") {
    World world = makeWorld(1s);
    LaunchRequest request = leo();
    request.propellantKg = 0.5;             // almost nothing
    request.maxThrustNewtons = 400.0;
    const auto id = world.launch(request);

    // Ask for far longer than the propellant can sustain.
    REQUIRE(world.commandBurn(id, ThrustCommand::Frame::Prograde, 1.0, 600s));
    world.step(600);

    const auto* craft = world.find(id);
    REQUIRE_FALSE(craft->thrust.active);
    REQUIRE(craft->propellantKg == 0.0);
    REQUIRE(hasEvent(world.peekEvents(), SimEvent::Kind::PropellantExhausted));
    // ...and it did not go negative, which would give free delta-v.
    REQUIRE(craft->propellantKg >= 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Handles and events
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a stale handle is detected, not silently reused", "[sim][world]") {
    // The reason ids carry a generation counter. An index alone would start
    // pointing at whoever moved into the slot the moment the container reorders,
    // and it will reorder once conjunction detection sorts spatially.
    World world = makeWorld();
    const auto id = world.launch(leo());
    REQUIRE(world.find(id) != nullptr);

    auto forged = id;
    forged.generation += 1;
    REQUIRE(world.find(forged) == nullptr);

    REQUIRE(world.find(omma::SpacecraftId::invalid()) == nullptr);
    auto outOfRange = id;
    outOfRange.index = 9999;
    REQUIRE(world.find(outOfRange) == nullptr);
}

TEST_CASE("events are drained, not duplicated", "[sim][world]") {
    World world = makeWorld();
    world.launch(leo());
    const auto first = world.drainEvents();
    REQUIRE_FALSE(first.empty());
    REQUIRE(world.drainEvents().empty());
}

TEST_CASE("thrust directions are orthogonal where they should be", "[sim][spacecraft]") {
    const omma::StateVector state{omma::Vec3{7.0e6, 0.0, 0.0},
                                  omma::Vec3{0.0, 7546.0, 0.0}};

    const auto direction = [&](ThrustCommand::Frame frame) {
        ThrustCommand command{};
        command.frame = frame;
        return omma::thrustDirection(command, state);
    };

    const auto prograde = direction(ThrustCommand::Frame::Prograde);
    const auto normal = direction(ThrustCommand::Frame::Normal);
    const auto radial = direction(ThrustCommand::Frame::RadialOut);

    // The three form a right-handed orthogonal frame; a mixed-up cross product
    // would break one of these dots.
    REQUIRE_THAT(dot(prograde, normal), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(dot(prograde, radial), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(dot(normal, radial), WithinAbs(0.0, 1e-12));

    REQUIRE(direction(ThrustCommand::Frame::Retrograde) == -prograde);
    REQUIRE(direction(ThrustCommand::Frame::AntiNormal) == -normal);
    REQUIRE(direction(ThrustCommand::Frame::RadialIn) == -radial);
}
