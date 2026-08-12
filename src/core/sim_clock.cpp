#include "core/sim_clock.hpp"

#include <cassert>
#include <cmath>

namespace omma {

SimClock::SimClock(Duration fixedStep, Epoch start) noexcept
    : start_{start}, fixedStep_{fixedStep} {
    assert(fixedStep.count() > 0 && "fixed step must be positive");
}

void SimClock::step(std::int64_t n) noexcept {
    assert(n >= 0 && "use seekToTick() to move backwards");
    ticks_ += n;
}

StepPacer::StepPacer(Duration fixedStep, std::int64_t maxStepsPerFrame) noexcept
    : fixedStepSeconds_{toSeconds(fixedStep)},
      maxStepsPerFrame_{maxStepsPerFrame > 0 ? maxStepsPerFrame : 1} {
    assert(fixedStep.count() > 0 && "fixed step must be positive");
}

std::int64_t StepPacer::stepsForFrame(double realSecondsElapsed, double warp) noexcept {
    clamped_ = false;

    // Reject garbage at the door: one NaN in the accumulator makes every later
    // frame return zero steps — the sim freezes with no error anywhere.
    if (!std::isfinite(realSecondsElapsed) || !std::isfinite(warp)) {
        return 0;
    }
    // Negative frame time: the wall clock went backwards (NTP, VM suspend).
    if (realSecondsElapsed <= 0.0 || warp <= 0.0) {
        return 0;
    }

    accumulator_ += realSecondsElapsed * warp;

    const double wholeSteps = std::floor(accumulator_ / fixedStepSeconds_);

    // Guard the cast: at extreme warp the quotient can exceed int64 range, and
    // an overflowing double-to-int64 conversion is undefined behaviour.
    const auto cap = static_cast<double>(maxStepsPerFrame_);
    if (wholeSteps >= cap) {
        clamped_ = true;
        accumulator_ = 0.0;   // abandon the backlog rather than chase it
        return maxStepsPerFrame_;
    }

    const auto steps = static_cast<std::int64_t>(wholeSteps);
    accumulator_ -= static_cast<double>(steps) * fixedStepSeconds_;

    // Floating-point subtraction can leave a hair below zero; clamp so
    // interpolationAlpha() never returns a negative fraction.
    if (accumulator_ < 0.0) {
        accumulator_ = 0.0;
    }
    return steps;
}

void StepPacer::reset() noexcept {
    accumulator_ = 0.0;
    clamped_ = false;
}

void StepPacer::setMaxStepsPerFrame(std::int64_t n) noexcept {
    maxStepsPerFrame_ = n > 0 ? n : 1;
}

}  // namespace omma
