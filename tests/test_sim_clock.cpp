#include "core/sim_clock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::Epoch;
using omma::SimClock;
using omma::StepPacer;

TEST_CASE("SimClock does not drift, ever", "[core][clock][determinism]") {
    // A million steps of 100 ms. Simulated elapsed time must be exactly
    // 100000 s, because the epoch is derived from the tick count rather than
    // accumulated into.
    SimClock clock{100ms};
    for (int i = 0; i < 1'000'000; ++i) {
        clock.step();
    }

    REQUIRE(clock.tickCount() == 1'000'000);
    REQUIRE(clock.elapsed() == std::chrono::seconds{100'000});
    REQUIRE(clock.now() == Epoch::j2000() + std::chrono::seconds{100'000});
}

TEST_CASE("stepping n times equals one step of n", "[core][clock][determinism]") {
    // If these two ever disagreed, a replay driven by recorded step counts
    // would diverge from the live run that produced it.
    SimClock byOnes{1s};
    SimClock byBatch{1s};

    for (int i = 0; i < 5000; ++i) {
        byOnes.step();
    }
    byBatch.step(5000);

    REQUIRE(byOnes.now() == byBatch.now());
    REQUIRE(byOnes.tickCount() == byBatch.tickCount());
    REQUIRE(byOnes.elapsed() == byBatch.elapsed());
}

TEST_CASE("SimClock can seek, because time is derived not accumulated", "[core][clock]") {
    SimClock clock{1s};
    clock.step(1000);
    const Epoch atThousand = clock.now();

    clock.seekToTick(0);
    REQUIRE(clock.now() == Epoch::j2000());

    clock.seekToTick(1000);
    REQUIRE(clock.now() == atThousand);   // exactly, not approximately

    // Seeking backwards past the start is legal and gives a pre-start epoch.
    clock.seekToTick(-500);
    REQUIRE(clock.now() == Epoch::j2000() - 500s);
}

TEST_CASE("SimClock honours a non-default start epoch", "[core][clock]") {
    const Epoch launch = Epoch::fromCivil(omma::CivilTime{2026, 8, 6, 12, 0, 0.0});
    SimClock clock{60s, launch};

    clock.step(60);   // one hour of simulated time
    REQUIRE(clock.now() == launch + 1h);
    REQUIRE(clock.startEpoch() == launch);
    REQUIRE_THAT(clock.fixedStepSeconds(), WithinRel(60.0, 1e-15));
}

// ─────────────────────────────────────────────────────────────────────────────
// StepPacer — the wall-clock side of the boundary.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("StepPacer converts wall time and warp into whole steps", "[core][pacer]") {
    StepPacer pacer{1s};

    SECTION("at 1x, a one-second frame is one step") {
        REQUIRE(pacer.stepsForFrame(1.0, 1.0) == 1);
    }

    SECTION("at 1x, a 16.7 ms frame is no steps but accumulates") {
        REQUIRE(pacer.stepsForFrame(0.0167, 1.0) == 0);
        REQUIRE_THAT(pacer.remainderSeconds(), WithinAbs(0.0167, 1e-12));
    }

    SECTION("warp multiplies") {
        REQUIRE(pacer.stepsForFrame(0.1, 100.0) == 10);
    }

    SECTION("warp 0 is paused") {
        REQUIRE(pacer.stepsForFrame(1.0, 0.0) == 0);
        REQUIRE(pacer.remainderSeconds() == 0.0);
    }
}

TEST_CASE("StepPacer conserves time across many ragged frames", "[core][pacer]") {
    // The remainder must be carried, not dropped. Sixty frames of an awkward
    // duration should produce very nearly the right total, with the shortfall
    // bounded by one step.
    constexpr double kFrame = 1.0 / 60.0;
    constexpr double kWarp = 137.0;          // deliberately not a round number
    constexpr int kFrames = 600;

    StepPacer pacer{1s};
    std::int64_t total = 0;
    for (int i = 0; i < kFrames; ++i) {
        total += pacer.stepsForFrame(kFrame, kWarp);
    }

    const double expected = kFrame * kWarp * kFrames;
    const double delivered = static_cast<double>(total) + pacer.remainderSeconds();
    REQUIRE_THAT(delivered, WithinRel(expected, 1e-9));

    // And the un-delivered part is always less than one whole step.
    REQUIRE(pacer.remainderSeconds() < 1.0);
    REQUIRE(pacer.remainderSeconds() >= 0.0);
}

TEST_CASE("StepPacer clamps rather than entering the spiral of death",
          "[core][pacer][determinism]") {
    StepPacer pacer{1s, /*maxStepsPerFrame=*/1000};

    // A frame that stalled for a full second at 1,000,000x warp asks for a
    // million steps. We give it a thousand and let simulated time fall behind.
    const std::int64_t steps = pacer.stepsForFrame(1.0, 1'000'000.0);
    REQUIRE(steps == 1000);
    REQUIRE(pacer.lastFrameWasClamped());

    // Critically, the backlog is discarded. If it were carried, the next frame
    // would clamp too, and the one after that, forever.
    REQUIRE(pacer.remainderSeconds() == 0.0);
    REQUIRE(pacer.stepsForFrame(0.001, 1.0) == 0);
    REQUIRE_FALSE(pacer.lastFrameWasClamped());
}

TEST_CASE("StepPacer refuses to be poisoned by bad input", "[core][pacer]") {
    StepPacer pacer{1s};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // A single NaN in the accumulator would make every future frame return
    // zero steps, and the simulator would appear to freeze with no error
    // reported anywhere. Reject at the door instead.
    REQUIRE(pacer.stepsForFrame(nan, 1.0) == 0);
    REQUIRE(pacer.stepsForFrame(1.0, nan) == 0);
    REQUIRE(pacer.stepsForFrame(inf, 1.0) == 0);
    REQUIRE(std::isfinite(pacer.remainderSeconds()));

    // The pacer is still healthy afterwards.
    REQUIRE(pacer.stepsForFrame(2.0, 1.0) == 2);

    SECTION("a backwards wall clock is ignored, not integrated") {
        // NTP corrections and VM suspends really do hand you negative deltas.
        StepPacer p{1s};
        REQUIRE(p.stepsForFrame(-5.0, 1.0) == 0);
        REQUIRE(p.remainderSeconds() == 0.0);
    }
}

TEST_CASE("interpolationAlpha stays in [0,1)", "[core][pacer]") {
    StepPacer pacer{1s};
    for (int i = 0; i < 1000; ++i) {
        static_cast<void>(pacer.stepsForFrame(1.0 / 60.0, 7.3));
        const double alpha = pacer.interpolationAlpha();
        REQUIRE(alpha >= 0.0);
        REQUIRE(alpha < 1.0);
    }
}

TEST_CASE("a recorded step sequence replays identically", "[core][clock][determinism]") {
    // This is what the whole SimClock/StepPacer split buys: capture the step
    // counts from a live, frame-rate-dependent run, and reproduce the exact
    // same simulated timeline later with no wall clock involved at all.
    StepPacer pacer{50ms};
    std::vector<std::int64_t> recorded;
    const double raggedFrames[] = {0.016, 0.033, 0.008, 0.120, 0.016, 0.016, 0.051};

    SimClock live{50ms};
    for (int repeat = 0; repeat < 20; ++repeat) {
        for (const double frame : raggedFrames) {
            const std::int64_t n = pacer.stepsForFrame(frame, 12.5);
            recorded.push_back(n);
            live.step(n);
        }
    }

    SimClock replay{50ms};
    for (const std::int64_t n : recorded) {
        replay.step(n);
    }

    REQUIRE(replay.tickCount() == live.tickCount());
    REQUIRE(replay.now() == live.now());
}
