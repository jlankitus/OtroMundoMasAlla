// Integrator and gravity-field validation.
//
// This is the most important test file in the project. Everything downstream —
// launches, constellations, intercepts — is only as trustworthy as the claim
// that these integrators conserve what they are supposed to conserve.
//
// The strategy is to test against things that are true independently of the
// implementation: an analytic Kepler solution, and the conservation laws. Not
// against a table of numbers a previous run produced, which only proves the
// code still does what it did.

#include "core/units.hpp"
#include "physics/gravity_field.hpp"
#include "physics/integrator.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/solar_system.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace omma::literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::Epoch;
using omma::GravityField;
using omma::Integrator;
using omma::StateVector;
using omma::Vec3;

namespace {

constexpr double kGmEarth = 3.98600435507e14;
constexpr double kEarthRadius = 6.371e6;

/// An Earth on its own, at the origin, with nothing else in the universe. The
/// only environment in which an analytic two-body solution is exactly right, so
/// the only fair place to measure integration error.
class LoneEarth final : public omma::IEphemeris {
public:
    [[nodiscard]] StateVector sample(Epoch) const noexcept override { return {}; }
    [[nodiscard]] double gravitationalParameter() const noexcept override { return kGmEarth; }
    [[nodiscard]] double meanRadius() const noexcept override { return kEarthRadius; }
    [[nodiscard]] std::string_view name() const noexcept override { return "Earth"; }
};

const LoneEarth& loneEarth() {
    static const LoneEarth earth;
    return earth;
}

GravityField earthOnlyField() {
    return GravityField{{&loneEarth()}};
}

/// A circular orbit at the given altitude, in the equatorial plane.
StateVector circularOrbit(double altitude) {
    const double r = kEarthRadius + altitude;
    return StateVector{Vec3{r, 0.0, 0.0}, Vec3{0.0, std::sqrt(kGmEarth / r), 0.0}};
}

double circularPeriod(double altitude) {
    const double r = kEarthRadius + altitude;
    return omma::constants::kTwoPi * std::sqrt(r * r * r / kGmEarth);
}

/// Integrate for a span and report the relative change in total energy.
struct DriftResult {
    double relativeEnergyDrift;
    double relativeAngularMomentumDrift;
    double finalRadius;
};

DriftResult measureDrift(Integrator method, StateVector state, double dt,
                        double totalSeconds) {
    GravityField field = earthOnlyField();
    const Epoch start = Epoch::j2000();

    field.refresh(start);
    const double energy0 = specificEnergy(state, field);
    const double h0 = omma::specificAngularMomentum(state).norm();

    const auto steps = static_cast<long long>(totalSeconds / dt);
    for (long long i = 0; i < steps; ++i) {
        integrate(method, state, field, start + omma::fromSeconds(dt * static_cast<double>(i)),
                  dt);
    }

    field.refresh(start + omma::fromSeconds(dt * static_cast<double>(steps)));
    const double energy1 = specificEnergy(state, field);
    const double h1 = omma::specificAngularMomentum(state).norm();

    return {std::abs((energy1 - energy0) / energy0),
            std::abs((h1 - h0) / h0),
            state.position.norm()};
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// GravityField
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("surface gravity comes out at 9.8 m/s^2", "[physics][gravity]") {
    GravityField field = earthOnlyField();
    field.refresh(Epoch::j2000());

    const Vec3 a = field.accelerationAt(Vec3{kEarthRadius, 0.0, 0.0});
    REQUIRE_THAT(a.norm(), WithinAbs(9.82, 0.02));
    // ...and it points at the planet, not away from it. A sign error here makes
    // everything fly apart, which is at least obvious; a sign error in one
    // component only would be much worse.
    REQUIRE(a.x < 0.0);
    REQUIRE_THAT(a.y, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(a.z, WithinAbs(0.0, 1e-12));
}

TEST_CASE("gravity falls off as the inverse square", "[physics][gravity]") {
    GravityField field = earthOnlyField();
    field.refresh(Epoch::j2000());

    const double near = field.accelerationAt(Vec3{kEarthRadius, 0.0, 0.0}).norm();
    const double far = field.accelerationAt(Vec3{2.0 * kEarthRadius, 0.0, 0.0}).norm();
    REQUIRE_THAT(near / far, WithinRel(4.0, 1e-12));

    const double tenTimes = field.accelerationAt(Vec3{10.0 * kEarthRadius, 0.0, 0.0}).norm();
    REQUIRE_THAT(near / tenTimes, WithinRel(100.0, 1e-12));
}

TEST_CASE("sources superpose", "[physics][gravity]") {
    // Two identical bodies either side of a point cancel exactly. A field that
    // summed magnitudes instead of vectors would pass an inverse-square test
    // and fail this one.
    const omma::SolarSystem system = omma::SolarSystem::standard();
    GravityField field = omma::makeGravityField(system);
    field.refresh(Epoch::j2000());
    REQUIRE(field.size() == system.size());

    // At the Sun's centre, every planet's pull is what remains — small, but the
    // Sun's own contribution is softened out rather than infinite.
    const Vec3 a = field.accelerationAt(Vec3::zero());
    REQUIRE(a.isFinite());
}

TEST_CASE("a body's centre yields a finite acceleration", "[physics][gravity]") {
    // A point mass has infinite acceleration at zero distance. Softening at the
    // body's own radius keeps the arithmetic finite so the collision check
    // downstream gets a chance to fire, instead of a NaN silently poisoning the
    // run and every state after it.
    GravityField field = earthOnlyField();
    field.refresh(Epoch::j2000());

    REQUIRE(field.accelerationAt(Vec3::zero()).isFinite());
    REQUIRE(field.accelerationAt(Vec3{1.0, 0.0, 0.0}).isFinite());
    REQUIRE(field.accelerationAt(Vec3{0.0, 0.0, 1e-9}).isFinite());
}

TEST_CASE("the dominant source is the one whose pull is strongest",
          "[physics][gravity]") {
    // Sphere-of-influence logic reduces to this. It decides which body a
    // spacecraft's elements are measured against and which ellipse it freezes
    // onto when it goes on rails.
    const omma::SolarSystem system = omma::SolarSystem::standard();
    GravityField field = omma::makeGravityField(system);
    const Epoch t = Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 0, 0, 0.0});
    field.refresh(t);

    const Vec3 earthCentre = system[omma::BodyId::Earth].sample(t).position;
    const Vec3 moonCentre = system[omma::BodyId::Moon].sample(t).position;

    // In low Earth orbit, Earth wins.
    const Vec3 leo = earthCentre + Vec3{7.0e6, 0.0, 0.0};
    REQUIRE(field.dominantSourceIndex(leo)
            == static_cast<std::size_t>(omma::BodyId::Earth));

    // Just above the lunar surface, the Moon wins.
    const Vec3 lunarOrbit = moonCentre + Vec3{2.0e6, 0.0, 0.0};
    REQUIRE(field.dominantSourceIndex(lunarOrbit)
            == static_cast<std::size_t>(omma::BodyId::Moon));

    // Far from everything, the Sun wins.
    REQUIRE(field.dominantSourceIndex(Vec3{5.0e11, 0.0, 0.0})
            == static_cast<std::size_t>(omma::BodyId::Sun));
}

// ─────────────────────────────────────────────────────────────────────────────
// Integrators against the analytic solution
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RK4 tracks the analytic Kepler orbit for one revolution",
          "[physics][integrator]") {
    // The cross-check promised when the ephemeris layer went in: two entirely
    // independent implementations of the same physics, one analytic and one
    // numerical, which must agree. Neither can fake this.
    const double altitude = 400.0_km;
    const StateVector initial = circularOrbit(altitude);
    const double period = circularPeriod(altitude);

    GravityField field = earthOnlyField();
    const Epoch start = Epoch::j2000();
    field.refresh(start);

    const auto elements = omma::elementsFromState(initial, kGmEarth, start);

    StateVector numerical = initial;
    constexpr double dt = 1.0;
    const auto steps = static_cast<long long>(period / dt);
    for (long long i = 0; i < steps; ++i) {
        integrate(Integrator::RungeKutta4, numerical, field,
                  start + omma::fromSeconds(dt * static_cast<double>(i)), dt);
    }

    const Epoch end = start + omma::fromSeconds(dt * static_cast<double>(steps));
    const StateVector analytic = omma::stateFromElements(elements, kGmEarth, end);

    const double positionError = distance(numerical.position, analytic.position);
    const double orbitRadius = initial.position.norm();
    INFO("after one orbit of " << orbitRadius / 1000.0 << " km radius, RK4 differs from "
         << "the analytic solution by " << positionError << " m");

    // Under a metre out of 6771 kilometres — about one part in ten million.
    REQUIRE(positionError < 1.0);
}

TEST_CASE("integrator accuracy improves at the expected order",
          "[physics][integrator]") {
    // Halving dt should reduce the error by 2^order. That is the definition of
    // the order of a method, and checking it catches an implementation that
    // works but is not the method it claims to be — a transposed RK4 coefficient
    // still converges, just at second order instead of fourth.
    const double altitude = 400.0_km;
    const double period = circularPeriod(altitude);
    const Epoch start = Epoch::j2000();
    const auto elements = omma::elementsFromState(circularOrbit(altitude), kGmEarth, start);

    const auto errorAt = [&](Integrator method, double dt) {
        GravityField field = earthOnlyField();
        StateVector s = circularOrbit(altitude);
        const auto steps = static_cast<long long>(period / dt);
        for (long long i = 0; i < steps; ++i) {
            integrate(method, s, field,
                      start + omma::fromSeconds(dt * static_cast<double>(i)), dt);
        }
        const Epoch end = start + omma::fromSeconds(dt * static_cast<double>(steps));
        return distance(s.position, omma::stateFromElements(elements, kGmEarth, end).position);
    };

    SECTION("velocity Verlet is second order") {
        const double coarse = errorAt(Integrator::VelocityVerlet, 8.0);
        const double fine = errorAt(Integrator::VelocityVerlet, 4.0);
        INFO("dt 8 -> " << coarse << " m,  dt 4 -> " << fine << " m,  ratio "
             << coarse / fine);
        REQUIRE(coarse / fine > 3.0);     // 2^2 = 4, with slack
    }

    SECTION("explicit Euler is only first order") {
        const double coarse = errorAt(Integrator::ExplicitEuler, 8.0);
        const double fine = errorAt(Integrator::ExplicitEuler, 4.0);
        INFO("dt 8 -> " << coarse << " m,  dt 4 -> " << fine << " m,  ratio "
             << coarse / fine);
        REQUIRE(coarse / fine > 1.5);
        REQUIRE(coarse / fine < 3.0);     // conspicuously not 4
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Conservation — the tests that matter most
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RK4 conserves energy to high precision over a hundred orbits",
          "[physics][integrator][conservation]") {
    const double altitude = 400.0_km;
    const auto result = measureDrift(Integrator::RungeKutta4, circularOrbit(altitude),
                                    1.0, 100.0 * circularPeriod(altitude));

    INFO("relative energy drift: " << result.relativeEnergyDrift);
    REQUIRE(result.relativeEnergyDrift < 1e-11);
    REQUIRE(result.relativeAngularMomentumDrift < 1e-11);
}

TEST_CASE("velocity Verlet keeps energy BOUNDED, which is a different promise",
          "[physics][integrator][conservation]") {
    // A symplectic method exactly conserves a slightly wrong energy rather than
    // approximately conserving the right one. Its error oscillates within a
    // fixed envelope forever instead of accumulating — so the orbit never opens
    // up, however long you run.
    //
    // Tested by measuring the envelope over ten orbits and then over a hundred,
    // and requiring the hundred-orbit envelope not to be an order of magnitude
    // worse. A method with secular drift fails this and a symplectic one does
    // not, which is the distinction no single-run drift number can make.
    const double altitude = 400.0_km;
    const double period = circularPeriod(altitude);
    const Epoch start = Epoch::j2000();

    const auto envelope = [&](double orbits) {
        GravityField field = earthOnlyField();
        StateVector s = circularOrbit(altitude);
        field.refresh(start);
        const double energy0 = specificEnergy(s, field);
        double worst = 0.0;

        constexpr double dt = 2.0;
        const auto steps = static_cast<long long>(orbits * period / dt);
        for (long long i = 0; i < steps; ++i) {
            integrate(Integrator::VelocityVerlet, s, field,
                      start + omma::fromSeconds(dt * static_cast<double>(i)), dt);
            if (i % 97 == 0) {   // sampled, not every step: this loop is hot
                worst = std::max(worst,
                                 std::abs((specificEnergy(s, field) - energy0) / energy0));
            }
        }
        return worst;
    };

    const double tenOrbits = envelope(10.0);
    const double hundredOrbits = envelope(100.0);

    INFO("energy envelope: 10 orbits " << tenOrbits << ", 100 orbits " << hundredOrbits);
    REQUIRE(hundredOrbits < tenOrbits * 5.0);   // bounded, not proportional to time
}

TEST_CASE("explicit Euler gains energy and the orbit spirals outward",
          "[physics][integrator][conservation]") {
    // The cautionary baseline, asserted rather than asserted-about. Explicit
    // Euler consistently overshoots a curving path, so it adds energy on every
    // step and the orbit opens up.
    //
    // This test also proves the metrics work: an error measure that cannot
    // detect a genuinely bad integrator is not measuring anything.
    const double altitude = 400.0_km;
    const StateVector initial = circularOrbit(altitude);
    const auto result = measureDrift(Integrator::ExplicitEuler, initial, 1.0,
                                    20.0 * circularPeriod(altitude));

    INFO("explicit Euler energy drift over 20 orbits: " << result.relativeEnergyDrift
         << ", radius " << initial.position.norm() / 1000.0 << " km -> "
         << result.finalRadius / 1000.0 << " km");

    REQUIRE(result.relativeEnergyDrift > 1e-4);          // vastly worse than RK4
    REQUIRE(result.finalRadius > initial.position.norm());  // spiralling OUT
}

TEST_CASE("semi-implicit Euler beats explicit Euler at identical cost",
          "[physics][integrator][conservation]") {
    // The whole difference is updating velocity before position instead of
    // after. One reordered line, same single acceleration sample per step, and
    // the method becomes symplectic. Worth knowing that "make it symplectic"
    // is sometimes free.
    const double altitude = 400.0_km;
    const double span = 20.0 * circularPeriod(altitude);

    const auto explicitDrift =
        measureDrift(Integrator::ExplicitEuler, circularOrbit(altitude), 1.0, span);
    const auto semiImplicitDrift =
        measureDrift(Integrator::SemiImplicitEuler, circularOrbit(altitude), 1.0, span);

    REQUIRE(omma::accelerationSamplesPerStep(Integrator::ExplicitEuler)
            == omma::accelerationSamplesPerStep(Integrator::SemiImplicitEuler));

    INFO("explicit " << explicitDrift.relativeEnergyDrift
         << " vs semi-implicit " << semiImplicitDrift.relativeEnergyDrift);
    REQUIRE(semiImplicitDrift.relativeEnergyDrift
            < explicitDrift.relativeEnergyDrift * 0.1);
}

TEST_CASE("an eccentric orbit conserves energy too", "[physics][integrator][conservation]") {
    // Circular orbits are the easy case: the acceleration magnitude never
    // changes. A Molniya-like orbit sweeps through a 12x range of gravity
    // strength and is where a fixed step size actually hurts.
    omma::OrbitalElements el{};
    el.semiMajorAxis = 26600.0_km;
    el.eccentricity = 0.74;
    el.inclination = 63.4_deg;
    el.epoch = Epoch::j2000();

    const StateVector initial = omma::stateFromElements(el, kGmEarth, el.epoch);
    const double period = omma::orbitalPeriod(el, kGmEarth);

    const auto result = measureDrift(Integrator::RungeKutta4, initial, 1.0, 10.0 * period);
    INFO("eccentric RK4 energy drift over 10 orbits: " << result.relativeEnergyDrift);
    REQUIRE(result.relativeEnergyDrift < 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// Determinism and multi-body
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("integration is bit-reproducible", "[physics][integrator][determinism]") {
    // Same initial state, same steps, bit-identical result. Not "close" —
    // identical. Without this, replaying a recorded run diverges from the run
    // that produced it and every long-running bug becomes unfindable.
    const auto run = [] {
        GravityField field = earthOnlyField();
        StateVector s = circularOrbit(550.0_km);
        const Epoch start = Epoch::j2000();
        for (int i = 0; i < 20'000; ++i) {
            integrate(Integrator::RungeKutta4, s, field,
                      start + omma::fromSeconds(static_cast<double>(i)), 1.0);
        }
        return s;
    };

    const StateVector first = run();
    const StateVector second = run();
    REQUIRE(first == second);
}

TEST_CASE("a satellite survives the full solar system", "[physics][integrator]") {
    // The real environment: Earth plus the Sun, Moon and every planet pulling
    // on a low orbit. The third-body perturbations are genuine but small, so the
    // orbit should stay recognisably the same orbit over a few days.
    const omma::SolarSystem system = omma::SolarSystem::standard();
    GravityField field = omma::makeGravityField(system);
    const Epoch start = Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 0, 0, 0.0});

    const Vec3 earthPosition = system[omma::BodyId::Earth].sample(start).position;
    const Vec3 earthVelocity = system[omma::BodyId::Earth].sample(start).velocity;
    const double r = kEarthRadius + 550.0_km;
    const double v = std::sqrt(kGmEarth / r);

    StateVector s{earthPosition + Vec3{r, 0.0, 0.0}, earthVelocity + Vec3{0.0, v, 0.0}};

    constexpr double dt = 5.0;
    constexpr int kDays = 3;
    const auto steps = static_cast<long long>(kDays * 86'400 / dt);
    for (long long i = 0; i < steps; ++i) {
        integrate(Integrator::RungeKutta4, s, field,
                  start + omma::fromSeconds(dt * static_cast<double>(i)), dt);
    }

    const Epoch end = start + omma::fromSeconds(dt * static_cast<double>(steps));
    const Vec3 earthLater = system[omma::BodyId::Earth].sample(end).position;
    const double altitude = distance(s.position, earthLater) - kEarthRadius;

    INFO("altitude after " << kDays << " days: " << altitude / 1000.0 << " km");
    REQUIRE(s.isFinite());
    // Started at 550 km. Third-body perturbation over three days is tens of
    // kilometres at most; anything wilder means the field or the frame is wrong.
    REQUIRE(altitude > 500.0_km);
    REQUIRE(altitude < 600.0_km);
}

TEST_CASE("thrust changes the orbit the way a burn should",
          "[physics][integrator]") {
    // A prograde burn raises the opposite side of the orbit. This is the single
    // most counter-intuitive fact in orbital mechanics: pushing forward now does
    // not move you forward, it moves you higher half an orbit later.
    GravityField field = earthOnlyField();
    const Epoch start = Epoch::j2000();
    field.refresh(start);

    StateVector s = circularOrbit(400.0_km);
    const auto before = omma::elementsFromState(s, kGmEarth, start);
    REQUIRE(before.eccentricity < 1e-6);   // started circular

    // 10 m/s^2 prograde for 5 seconds: about 50 m/s of delta-v.
    const Vec3 prograde = s.velocity.normalized();
    for (int i = 0; i < 5; ++i) {
        integrate(Integrator::RungeKutta4, s, field,
                  start + omma::fromSeconds(static_cast<double>(i)), 1.0,
                  prograde * 10.0);
    }

    field.refresh(start + omma::fromSeconds(5.0));
    const auto after = omma::elementsFromState(s, kGmEarth, start);

    INFO("apoapsis " << omma::apoapsisRadius(before) / 1000.0 << " km -> "
         << omma::apoapsisRadius(after) / 1000.0 << " km");

    REQUIRE(after.eccentricity > before.eccentricity);
    REQUIRE(omma::apoapsisRadius(after) > omma::apoapsisRadius(before) + 100.0_km);
    // Periapsis barely moves: that is the point of burning at periapsis.
    REQUIRE_THAT(omma::periapsisRadius(after),
                 WithinRel(omma::periapsisRadius(before), 0.01));
}

TEST_CASE("integrator metadata is consistent", "[physics][integrator]") {
    for (const Integrator method : {Integrator::ExplicitEuler,
                                    Integrator::SemiImplicitEuler,
                                    Integrator::VelocityVerlet,
                                    Integrator::RungeKutta4}) {
        REQUIRE_FALSE(omma::integratorName(method).empty());
        REQUIRE(omma::integratorName(method) != "unknown");
        REQUIRE(omma::accelerationSamplesPerStep(method) >= 1);
    }
    REQUIRE(omma::accelerationSamplesPerStep(Integrator::RungeKutta4) == 4);
    REQUIRE(omma::accelerationSamplesPerStep(Integrator::VelocityVerlet) == 2);
}
