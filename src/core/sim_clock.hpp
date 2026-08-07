// ─────────────────────────────────────────────────────────────────────────────
// SimClock and StepPacer — the Game Loop pattern, with the determinism
// boundary drawn explicitly between them.
//
//     ┌──────────────────────────┐        ┌───────────────────────────────┐
//     │        StepPacer         │        │           SimClock            │
//     │  reads the wall clock    │───────▶│  never hears of the wall      │
//     │  owns a float accumulator│ how    │  exact integer tick count     │
//     │  NON-DETERMINISTIC       │ many   │  DETERMINISTIC                │
//     └──────────────────────────┘ steps? └───────────────────────────────┘
//
// Everything that makes a run unreproducible — frame timing, how long the OS
// decided to schedule us, whether the user tabbed away — lives on the left.
// The physics only ever sees "advance one fixed step", so a recorded sequence
// of step counts replays identically on any machine.
//
// This is the single most valuable structural decision in the whole simulator,
// and it costs two small classes.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"

#include <cstdint>

namespace omma {

/// Simulated time. Advances only in whole fixed steps, and only when told.
///
/// The current epoch is *derived* from the tick count rather than accumulated
/// into. That makes drift structurally impossible instead of merely unlikely,
/// and it means "tick 4,320,000" and "50 days in" are the same statement.
class SimClock {
public:
    /// \param fixedStep  the invariant physics timestep. Must be positive.
    /// \param start      the epoch at tick zero.
    explicit SimClock(Duration fixedStep, Epoch start = Epoch::j2000()) noexcept;

    /// Advance exactly one fixed step.
    void step() noexcept { ticks_ += 1; }

    /// Advance n fixed steps. Equivalent to calling step() n times, and
    /// guaranteed to produce the identical epoch — that equivalence is a test.
    void step(std::int64_t n) noexcept;

    /// Jump directly to a tick index. Used by replay and by scrubbing a
    /// timeline backwards, which only works because time is derived.
    void seekToTick(std::int64_t tick) noexcept { ticks_ = tick; }

    void reset() noexcept { ticks_ = 0; }

    [[nodiscard]] Epoch now() const noexcept { return start_ + fixedStep_ * ticks_; }
    [[nodiscard]] Epoch startEpoch() const noexcept { return start_; }
    [[nodiscard]] Duration elapsed() const noexcept { return fixedStep_ * ticks_; }

    [[nodiscard]] Duration fixedStep() const noexcept { return fixedStep_; }
    [[nodiscard]] double fixedStepSeconds() const noexcept { return toSeconds(fixedStep_); }
    [[nodiscard]] std::int64_t tickCount() const noexcept { return ticks_; }

private:
    Epoch        start_{};
    Duration     fixedStep_{};
    std::int64_t ticks_{0};
};

/// Converts elapsed wall-clock time and a warp factor into a whole number of
/// fixed steps, carrying the remainder.
///
/// This is the only place in the simulator that holds a floating-point time
/// accumulator, and the only place that cares how long a frame took.
class StepPacer {
public:
    /// \param fixedStep         must match the SimClock being driven.
    /// \param maxStepsPerFrame  the spiral-of-death guard; see stepsForFrame().
    explicit StepPacer(Duration fixedStep, std::int64_t maxStepsPerFrame = 100'000) noexcept;

    /// How many fixed steps to run for a frame that took \p realSecondsElapsed
    /// of wall time at time-warp \p warp.
    ///
    /// THE SPIRAL OF DEATH
    /// If a frame runs long, this returns more steps, which makes the next
    /// frame run longer, which returns still more steps. Left alone the
    /// simulation locks up while trying to catch up with a past it can never
    /// reach. The fix is to cap the steps and *let simulated time fall behind
    /// wall time* — a slow machine runs the same physics, just slower than
    /// real life. Correctness is preserved; only the illusion of real-time is
    /// lost, and that was never the guarantee.
    ///
    /// warp <= 0 returns 0 (paused). Non-finite inputs return 0 rather than
    /// poisoning the accumulator with NaN.
    [[nodiscard]] std::int64_t stepsForFrame(double realSecondsElapsed, double warp) noexcept;

    /// True if the most recent stepsForFrame() hit the cap, meaning simulated
    /// time is now falling behind. Worth surfacing on a HUD.
    [[nodiscard]] bool lastFrameWasClamped() const noexcept { return clamped_; }

    /// Leftover simulated time not yet consumed by a whole step, in seconds.
    /// Always in [0, fixedStepSeconds).
    [[nodiscard]] double remainderSeconds() const noexcept { return accumulator_; }

    /// Fraction of the way into the next step, in [0, 1). Renderers use this
    /// to interpolate between the last two physics states so that motion looks
    /// smooth even though the physics is quantised.
    [[nodiscard]] double interpolationAlpha() const noexcept {
        return accumulator_ / fixedStepSeconds_;
    }

    void reset() noexcept;

    [[nodiscard]] std::int64_t maxStepsPerFrame() const noexcept { return maxStepsPerFrame_; }
    void setMaxStepsPerFrame(std::int64_t n) noexcept;

private:
    double       fixedStepSeconds_{};
    double       accumulator_{0.0};
    std::int64_t maxStepsPerFrame_{100'000};
    bool         clamped_{false};
};

}  // namespace omma
