// ─────────────────────────────────────────────────────────────────────────────
// omma-headless — the graphics-free simulation driver.
//
// It grows into the scenario runner: load a scenario, step the world at a
// fixed dt, stream telemetry to disk, return non-zero if a mission constraint
// is violated. Until there is a world to run, it demonstrates the core, which
// keeps the demo honest — everything printed below is computed live by the
// same code the simulator will use.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/console.hpp"
#include "core/epoch.hpp"
#include "core/sim_clock.hpp"
#include "core/units.hpp"
#include "core/vec3.hpp"
#include "core/version.hpp"
#include "physics/gravity_field.hpp"
#include "physics/integrator.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/solar_system.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace std::chrono_literals;
using namespace omma::literals;
namespace con = omma::console;

namespace {

void rule(const char* title) {
    // Note: printf's field-width counts *bytes*, and box-drawing characters
    // are three bytes each in UTF-8. Any table that needs to line up therefore
    // uses ASCII inside format specifiers, and multibyte glyphs only where the
    // count is done by hand, as here.
    std::printf("\n%s%s-- %s ", con::bold().data(), con::cyan().data(), title);
    const std::size_t drawn = 3 + std::string(title).size() + 1;
    for (std::size_t i = drawn; i < 72; ++i) std::printf("-");
    std::printf("%s\n", con::reset().data());
}

void demoVectors() {
    rule("vectors");

    // A real low Earth orbit state vector: 400 km circular, moving east.
    const omma::Vec3 r{6371.0_km + 400.0_km, 0.0, 0.0};
    const omma::Vec3 v{0.0, 7.669_kmps, 0.0};

    std::printf("  position           r = (%13.1f, %13.1f, %13.1f) m\n", r.x, r.y, r.z);
    std::printf("  velocity           v = (%13.1f, %13.1f, %13.1f) m/s\n", v.x, v.y, v.z);
    std::printf("  |r|                  = %.3f km   (altitude %.1f km)\n",
                r.norm() / 1000.0, (r.norm() - 6371.0_km) / 1000.0);
    std::printf("  |v|                  = %.3f km/s\n", v.norm() / 1000.0);

    // Specific angular momentum. Constant for a two-body orbit, and the vector
    // that defines the orbital plane -- the first real orbital quantity this
    // codebase computes.
    const omma::Vec3 h = cross(r, v);
    std::printf("  h = r x v            = (%.3e, %.3e, %.3e) m^2/s\n", h.x, h.y, h.z);
    std::printf("  flight path angle    = %.4f deg   (0 means circular)\n",
                90.0 - omma::toDegrees(omma::angleBetween(r, v)));
}

void demoEpoch() {
    rule("epoch");

    const omma::Epoch j2000 = omma::Epoch::j2000();
    const omma::Epoch today = omma::Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 12, 0, 0.0});

    std::printf("  J2000                %s   JD %.4f\n",
                j2000.toString().c_str(), j2000.julianDate());
    std::printf("  today                %s   JD %.4f\n",
                today.toString().c_str(), today.julianDate());
    std::printf("  elapsed              %.2f days  (%.3f Julian years)\n",
                today.daysSinceJ2000(),
                today.secondsSinceJ2000() / omma::constants::kSecondsPerJulianYear);
    std::printf("  Epoch - Epoch        %lld ns  <- a Duration, and the only\n",
                static_cast<long long>((today - j2000).count()));
    std::printf("                       subtraction of two epochs that type-checks\n");
}

void demoNoDrift() {
    rule("fixed-step clock: one million ticks of 100 ms");

    constexpr int kIterations = 1'000'000;

    omma::SimClock clock{100ms};
    double naive = 0.0;
    for (int i = 0; i < kIterations; ++i) {
        clock.step();
        naive += 0.1;      // the tempting, wrong way
    }

    const double exact = static_cast<double>(clock.elapsed().count()) * 1e-9;
    const double drift = naive - exact;

    std::printf("  simulated span       %.4f hours\n", exact / 3600.0);
    std::printf("  int64 nanoseconds    %.12f s   <- exact, by construction\n", exact);
    std::printf("  double accumulator   %.12f s\n", naive);
    std::printf("  %sdrift                %+.3e s after %d additions%s\n",
                con::yellow().data(), drift, kIterations, con::reset().data());

    // Linear extrapolation is generous to the naive version -- real drift is
    // worse, because the error per addition grows with the accumulator.
    const double tenYears = 10.0 * omma::constants::kSecondsPerJulianYear;
    std::printf("  projected over 10 y  %+.3e s  (>= this; error grows with magnitude)\n",
                drift * (tenYears / exact));
    std::printf("  ticks                %lld   <- the tick count IS the time;\n",
                static_cast<long long>(clock.tickCount()));
    std::printf("                       now() is derived from it, never accumulated\n");
}

void demoTimeWarp() {
    rule("time warp: fixed steps per 60 Hz frame");

    constexpr double kFrame = 1.0 / 60.0;
    struct Row { double warp; const char* label; };
    constexpr Row kRows[] = {
        {1.0,         "real time"},
        {60.0,        "1 min / s"},
        {3'600.0,     "1 hour / s"},
        {86'400.0,    "1 day / s"},
        {1'000'000.0, "11.6 days / s"},
    };

    std::printf("    %12s  %-14s %16s %13s\n", "warp", "", "sim time/frame", "steps/frame");
    std::printf("    %12s  %-14s %16s %13s\n",
                "------------", "--------------", "----------------", "-------------");

    for (const Row& row : kRows) {
        omma::StepPacer pacer{1s};
        // Average over a second of frames: at low warp most frames yield zero
        // steps and one frame yields one, which is exactly the point of
        // carrying a remainder.
        std::int64_t total = 0;
        for (int f = 0; f < 60; ++f) total += pacer.stepsForFrame(kFrame, row.warp);

        std::printf("    %11.0fx  %-14s %14.3f s %13.1f\n",
                    row.warp, row.label, kFrame * row.warp,
                    static_cast<double>(total) / 60.0);
    }
    std::printf("\n  dt is 1 s on every row. Warp changes how many steps we run,\n");
    std::printf("  never how big a step is -- that is what keeps runs reproducible.\n");
}

void demoSpiralGuard() {
    rule("spiral of death guard");

    omma::StepPacer pacer{1s, /*maxStepsPerFrame=*/100'000};

    const std::int64_t asked = 1'000'000;
    const std::int64_t ran = pacer.stepsForFrame(1.0, static_cast<double>(asked));

    std::printf("  a frame stalled for 1.0 s at %lldx warp\n", static_cast<long long>(asked));
    std::printf("  steps requested      %lld\n", static_cast<long long>(asked));
    std::printf("  steps actually run   %lld   %s%s%s\n",
                static_cast<long long>(ran), con::yellow().data(),
                pacer.lastFrameWasClamped() ? "CLAMPED" : "", con::reset().data());
    std::printf("  backlog carried      %.1f s   <- discarded on purpose\n",
                pacer.remainderSeconds());
    std::printf("\n  Carrying the backlog would make the next frame ask for even more\n");
    std::printf("  steps, and the one after that more still. Simulated time falls\n");
    std::printf("  behind the wall clock instead. The physics stays correct; only\n");
    std::printf("  the illusion of real-time is lost, and that was never promised.\n");
}

void demoReplay() {
    rule("determinism: record and replay");

    const double raggedFrames[] = {0.016, 0.033, 0.008, 0.120, 0.016, 0.051};
    omma::StepPacer pacer{50ms};
    omma::SimClock live{50ms};
    std::int64_t recorded[120]{};

    std::size_t n = 0;
    for (int repeat = 0; repeat < 20; ++repeat) {
        for (const double frame : raggedFrames) {
            const std::int64_t steps = pacer.stepsForFrame(frame, 12.5);
            recorded[n++] = steps;
            live.step(steps);
        }
    }

    omma::SimClock replay{50ms};
    for (std::size_t i = 0; i < n; ++i) replay.step(recorded[i]);

    const bool identical = (live.now() == replay.now());

    std::printf("  %zu frames of jittery wall time recorded at 12.5x warp\n", n);
    std::printf("  live run             tick %lld, %s\n",
                static_cast<long long>(live.tickCount()), live.now().toString().c_str());
    std::printf("  replayed run         tick %lld, %s\n",
                static_cast<long long>(replay.tickCount()), replay.now().toString().c_str());
    std::printf("  %sidentical            %s%s\n",
                identical ? con::green().data() : con::red().data(),
                identical ? "yes -- to the nanosecond" : "NO",
                con::reset().data());
}

void demoSolarSystem(const omma::SolarSystem& system, omma::Epoch t) {
    rule("the solar system, right now");

    std::printf("  epoch %s UTC   (JD %.5f)\n\n", t.toString().c_str(), t.julianDate());
    std::printf("    %-8s %10s %10s %9s %8s %7s %10s\n",
                "body", "r [au]", "r [Gm]", "v [km/s]", "period", "ecc", "true anom");
    std::printf("    %-8s %10s %10s %9s %8s %7s %10s\n",
                "--------", "----------", "----------", "---------",
                "--------", "-------", "----------");

    for (const auto& body : system.bodies()) {
        const auto state = body->sample(t);
        const double rAu = state.position.norm() / omma::constants::kAu;
        const double speedKms = state.velocity.norm() / 1000.0;

        // The Sun is a FixedEphemeris and has no orbit to report. Rather than
        // ask the interface for something it cannot provide, we ask whether
        // this particular implementation happens to be a Kepler body. A
        // dynamic_cast in a display path is fine; one in a physics inner loop
        // would be a design smell worth fixing.
        const auto* kepler = dynamic_cast<const omma::KeplerEphemeris*>(body.get());
        if (kepler == nullptr) {
            std::printf("    %-8s %10.4f %10.4f %9.3f %8s %7s %10s\n",
                        body->name().data(), rAu, state.position.norm() / 1e9,
                        speedKms, "-", "-", "-");
            continue;
        }

        const auto elements = kepler->elementsAt(t);
        const double periodDays =
            kepler->orbitalPeriodSeconds(t) / omma::constants::kSecondsPerDay;
        const double E = omma::solveKeplerEquation(elements.meanAnomalyAtEpoch,
                                                   elements.eccentricity);
        const double nu = omma::wrapToTwoPi(
            omma::trueAnomalyFromEccentric(E, elements.eccentricity));

        char periodText[16];
        if (periodDays < 1000.0) {
            std::snprintf(periodText, sizeof(periodText), "%.1f d", periodDays);
        } else {
            std::snprintf(periodText, sizeof(periodText), "%.2f y",
                          periodDays / 365.25);
        }

        std::printf("    %-8s %10.4f %10.4f %9.3f %8s %7.4f %9.2f\n",
                    body->name().data(), rAu, state.position.norm() / 1e9, speedKms,
                    periodText, elements.eccentricity, omma::toDegrees(nu));
    }

    // Distances between bodies, which is what a mission planner actually cares
    // about and what a heliocentric table does not show you.
    const auto earth = system[omma::BodyId::Earth].sample(t);
    const auto moon = system[omma::BodyId::Moon].sample(t);
    const auto mars = system[omma::BodyId::Mars].sample(t);
    const double lightSecond = omma::constants::kSpeedOfLight;

    std::printf("\n  Earth to Moon        %9.0f km   (%.2f light-seconds)\n",
                distance(earth.position, moon.position) / 1000.0,
                distance(earth.position, moon.position) / lightSecond);
    std::printf("  Earth to Mars        %9.3f Gm   (%.1f light-minutes)\n",
                distance(earth.position, mars.position) / 1e9,
                distance(earth.position, mars.position) / lightSecond / 60.0);
}

void demoAnalyticWarp(const omma::SolarSystem& system, omma::Epoch t) {
    rule("analytic propagation: any epoch, one call");

    std::printf("  Kepler bodies are a pure function of time, so jumping ahead\n");
    std::printf("  costs exactly what advancing one second costs. This is what\n");
    std::printf("  makes unbounded time warp possible.\n\n");

    const auto& jupiter = system[omma::BodyId::Jupiter];
    std::printf("    %-22s %10s %11s\n", "epoch", "r [au]", "long [deg]");
    std::printf("    %-22s %10s %11s\n", "----------------------", "----------",
                "-----------");

    for (const int years : {0, 1, 6, 12, 24, -50}) {
        const omma::Epoch when = t + std::chrono::hours{24 * 365 * years};
        const auto p = jupiter.sample(when).position;
        std::printf("    %-22s %10.4f %11.2f\n",
                    when.toString().substr(0, 10).c_str(),
                    p.norm() / omma::constants::kAu,
                    omma::toDegrees(omma::wrapToTwoPi(std::atan2(p.y, p.x))));
    }
    std::printf("\n  Jupiter's period is 11.86 years, so the +12 year row should\n");
    std::printf("  land close to the +0 row. It does, and no integration ran.\n");
}

void demoConservation(const omma::SolarSystem& system, omma::Epoch t) {
    rule("conservation: is the model actually right?");

    // Specific orbital energy and angular momentum are constant on a Kepler
    // orbit. Sampling them a year apart and finding them unchanged is an
    // independent check on the propagator, and the same check will catch an
    // integrator that leaks energy once we have one.
    const auto& earth = system[omma::BodyId::Earth];
    const double gm = system[omma::BodyId::Sun].gravitationalParameter()
                    + earth.gravitationalParameter();

    const auto s0 = earth.sample(t);
    const auto s1 = earth.sample(t + std::chrono::hours{24 * 183});

    const double e0 = omma::specificOrbitalEnergy(s0, gm);
    const double e1 = omma::specificOrbitalEnergy(s1, gm);
    const auto h0 = omma::specificAngularMomentum(s0);
    const auto h1 = omma::specificAngularMomentum(s1);

    std::printf("  Earth, sampled six months apart:\n");
    std::printf("    radius               %.6f au  ->  %.6f au   (it moved)\n",
                s0.position.norm() / omma::constants::kAu,
                s1.position.norm() / omma::constants::kAu);
    std::printf("    speed                %.4f km/s  ->  %.4f km/s\n",
                s0.velocity.norm() / 1000.0, s1.velocity.norm() / 1000.0);
    std::printf("    specific energy      %.9e  ->  %.9e J/kg\n", e0, e1);
    std::printf("    %srelative change      %.2e   <- conserved%s\n",
                con::green().data(), std::abs((e1 - e0) / e0), con::reset().data());
    std::printf("    |h| relative change  %.2e   <- conserved\n",
                std::abs((h1.norm() - h0.norm()) / h0.norm()));
}

void demoIntegrators() {
    rule("integrator bake-off: 50 orbits of a 400 km circular orbit");

    // A lone Earth, so the analytic two-body solution is exactly right and any
    // difference is integration error rather than a real perturbation.
    class LoneEarth final : public omma::IEphemeris {
    public:
        [[nodiscard]] omma::StateVector sample(omma::Epoch) const noexcept override { return {}; }
        [[nodiscard]] double gravitationalParameter() const noexcept override {
            return 3.98600435507e14;
        }
        [[nodiscard]] double meanRadius() const noexcept override { return 6.371e6; }
        [[nodiscard]] std::string_view name() const noexcept override { return "Earth"; }
    };
    static const LoneEarth earth;

    constexpr double kGm = 3.98600435507e14;
    const double r0 = 6.371e6 + 400.0_km;
    const double v0 = std::sqrt(kGm / r0);
    const double period = omma::constants::kTwoPi * std::sqrt(r0 * r0 * r0 / kGm);
    const omma::StateVector initial{omma::Vec3{r0, 0.0, 0.0}, omma::Vec3{0.0, v0, 0.0}};
    const omma::Epoch start = omma::Epoch::j2000();

    // The analytic answer to compare against.
    const auto elements = omma::elementsFromState(initial, kGm, start);

    std::printf("    %-21s %7s %13s %14s %13s\n",
                "method", "samples", "energy drift", "position err", "final alt");
    std::printf("    %-21s %7s %13s %14s %13s\n",
                "---------------------", "-------", "-------------",
                "--------------", "-------------");

    constexpr double dt = 2.0;
    constexpr double kOrbits = 50.0;

    for (const omma::Integrator method : {omma::Integrator::ExplicitEuler,
                                          omma::Integrator::SemiImplicitEuler,
                                          omma::Integrator::VelocityVerlet,
                                          omma::Integrator::RungeKutta4}) {
        omma::GravityField field{{&earth}};
        omma::StateVector s = initial;

        field.refresh(start);
        const double energy0 = omma::specificEnergy(s, field);

        const auto steps = static_cast<long long>(kOrbits * period / dt);
        for (long long i = 0; i < steps; ++i) {
            omma::integrate(method, s, field,
                            start + omma::fromSeconds(dt * static_cast<double>(i)), dt);
        }

        const omma::Epoch end = start + omma::fromSeconds(dt * static_cast<double>(steps));
        field.refresh(end);
        const double drift = std::abs((omma::specificEnergy(s, field) - energy0) / energy0);
        const double positionError =
            distance(s.position, omma::stateFromElements(elements, kGm, end).position);
        const double altitude = (s.position.norm() - 6.371e6) / 1000.0;

        const bool bad = drift > 1e-6;
        std::printf("    %-21s %7d %s%13.3e%s %11.1f km %10.1f km\n",
                    omma::integratorName(method).data(),
                    omma::accelerationSamplesPerStep(method),
                    bad ? con::red().data() : con::green().data(), drift,
                    con::reset().data(),
                    positionError / 1000.0, altitude);
    }

    std::printf("\n  Started at 400.0 km. Explicit Euler consistently overshoots a\n");
    std::printf("  curving path, so it GAINS energy every step and spirals outward.\n");
    std::printf("  Semi-implicit Euler differs by one reordered line -- velocity\n");
    std::printf("  before position -- which makes it symplectic, at identical cost.\n");
    std::printf("  Verlet's error oscillates within a fixed envelope forever;\n");
    std::printf("  RK4's is far smaller but accumulates slowly. Neither is simply\n");
    std::printf("  better: RK4 wins over hours, symplectic wins over eons.\n");
}

void demoBurn() {
    rule("a prograde burn: pushing forward makes you go higher, later");

    class LoneEarth final : public omma::IEphemeris {
    public:
        [[nodiscard]] omma::StateVector sample(omma::Epoch) const noexcept override { return {}; }
        [[nodiscard]] double gravitationalParameter() const noexcept override {
            return 3.98600435507e14;
        }
        [[nodiscard]] double meanRadius() const noexcept override { return 6.371e6; }
        [[nodiscard]] std::string_view name() const noexcept override { return "Earth"; }
    };
    static const LoneEarth earth;

    constexpr double kGm = 3.98600435507e14;
    const double r0 = 6.371e6 + 400.0_km;
    omma::StateVector s{omma::Vec3{r0, 0.0, 0.0},
                        omma::Vec3{0.0, std::sqrt(kGm / r0), 0.0}};

    omma::GravityField field{{&earth}};
    const omma::Epoch start = omma::Epoch::j2000();
    field.refresh(start);

    const auto before = omma::elementsFromState(s, kGm, start);
    std::printf("  before      alt %.1f km circular, e = %.6f\n",
                (s.position.norm() - 6.371e6) / 1000.0, before.eccentricity);

    // 100 m/s of prograde delta-v, applied over 20 seconds.
    const omma::Vec3 prograde = s.velocity.normalized();
    constexpr double kDeltaV = 100.0;
    constexpr double kBurnSeconds = 20.0;
    for (int i = 0; i < static_cast<int>(kBurnSeconds); ++i) {
        omma::integrate(omma::Integrator::RungeKutta4, s, field,
                        start + omma::fromSeconds(static_cast<double>(i)), 1.0,
                        prograde * (kDeltaV / kBurnSeconds));
    }

    const auto after = omma::elementsFromState(s, kGm, start + omma::fromSeconds(kBurnSeconds));
    std::printf("  burn        %+.0f m/s prograde over %.0f s\n", kDeltaV, kBurnSeconds);
    std::printf("  after       e = %.6f,  peri %.1f km,  apo %.1f km\n",
                after.eccentricity,
                (omma::periapsisRadius(after) - 6.371e6) / 1000.0,
                (omma::apoapsisRadius(after) - 6.371e6) / 1000.0);
    std::printf("  period      %.2f min  ->  %.2f min\n",
                omma::orbitalPeriod(before, kGm) / 60.0,
                omma::orbitalPeriod(after, kGm) / 60.0);
    std::printf("\n  %sPeriapsis barely moved; apoapsis rose by %.0f km.%s\n",
                con::green().data(),
                (omma::apoapsisRadius(after) - omma::apoapsisRadius(before)) / 1000.0,
                con::reset().data());
    std::printf("  A burn changes the orbit on the OPPOSITE side. Thrust here and\n");
    std::printf("  you climb half an orbit from now -- which is why catching\n");
    std::printf("  something ahead of you means slowing down, not speeding up.\n");
}

}  // namespace

int main() {
    con::enableAnsi();

    std::printf("\n%s%s%s  ...  self-demo\n",
                con::bold().data(), omma::versionBanner().data(), con::reset().data());
    std::printf("no scenario loaded; showing what core and physics can do so far\n");

    const omma::SolarSystem system = omma::SolarSystem::standard();
    const omma::Epoch now = omma::Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 12, 0, 0.0});

    demoSolarSystem(system, now);
    demoIntegrators();
    demoBurn();
    demoAnalyticWarp(system, now);
    demoConservation(system, now);
    demoVectors();
    demoEpoch();
    demoNoDrift();
    demoTimeWarp();
    demoSpiralGuard();
    demoReplay();

    std::printf("\n");
    return 0;
}
