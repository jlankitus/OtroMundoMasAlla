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

#include <chrono>
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

}  // namespace

int main() {
    con::enableAnsi();

    std::printf("\n%s%s%s  ...  core self-demo\n",
                con::bold().data(), omma::versionBanner().data(), con::reset().data());
    std::printf("no scenario loaded; showing what core can do so far\n");

    demoVectors();
    demoEpoch();
    demoNoDrift();
    demoTimeWarp();
    demoSpiralGuard();
    demoReplay();

    std::printf("\n");
    return 0;
}
