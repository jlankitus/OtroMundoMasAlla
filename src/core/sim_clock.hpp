// ─────────────────────────────────────────────────────────────────────────────
// SimClock and StepPacer — the Game Loop pattern with the determinism boundary
// drawn between them. StepPacer reads the wall clock and owns the only float
// accumulator (non-deterministic); SimClock only ever hears "advance n fixed
// steps" (deterministic), so a recorded sequence of step counts replays
// identically on any machine. See docs/DESIGN.md §3.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"

#include <cstdint>

namespace omma {

/// Simulated time. Advances only in whole fixed steps, and only when told.
/// The tick count IS the time: now() is derived from it, never accumulated
/// into, so drift is structurally impossible.
class SimClock {
public:
    /// \param fixedStep  the invariant physics timestep. Must be positive.
    /// \param start      the epoch at tick zero.
    explicit SimClock(Duration fixedStep, Epoch start = Epoch::j2000()) noexcept;

    /// Advance exactly one fixed step.
    void step() noexcept { ticks_ += 1; }

    /// Advance n fixed steps; produces the identical epoch to n step() calls.
    void step(std::int64_t n) noexcept;

    /// Jump to a tick index (replay, scrubbing); works because time is derived.
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
/// fixed steps, carrying the remainder. The only place in the simulator that
/// holds a floating-point time accumulator or cares how long a frame took.
class StepPacer {
public:
    /// \param fixedStep         must match the SimClock being driven.
    /// \param maxStepsPerFrame  the spiral-of-death guard; see stepsForFrame().
    explicit StepPacer(Duration fixedStep, std::int64_t maxStepsPerFrame = 100'000) noexcept;

    /// How many fixed steps to run for a frame that took \p realSecondsElapsed
    /// of wall time at time-warp \p warp. Capped at maxStepsPerFrame with the
    /// backlog discarded — simulated time falls behind wall time rather than
    /// spiralling (see docs/DESIGN.md §3). warp <= 0 returns 0 (paused);
    /// non-finite inputs return 0 rather than poisoning the accumulator.
    [[nodiscard]] std::int64_t stepsForFrame(double realSecondsElapsed, double warp) noexcept;

    /// True if the most recent stepsForFrame() hit the cap, meaning simulated
    /// time is now falling behind. Worth surfacing on a HUD.
    [[nodiscard]] bool lastFrameWasClamped() const noexcept { return clamped_; }

    /// Leftover simulated time not yet consumed by a whole step, in seconds.
    /// Always in [0, fixedStepSeconds).
    [[nodiscard]] double remainderSeconds() const noexcept { return accumulator_; }

    /// Fraction of the way into the next step, in [0, 1). Renderers use this
    /// to interpolate between the last two physics states.
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
