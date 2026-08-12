// Scenarios: named, fully deterministic starting states, so the interesting
// thing is on screen the moment the program opens and a scenario name plus a
// frame count is a reproducible test case. The default is not an empty sky —
// the point of this simulator is spacecraft. See docs/DESIGN.md §8.
#pragma once

#include "sim/world.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace omma::app {

enum class Scenario { Empty, Leo, Constellation, Transfer };

[[nodiscard]] std::optional<Scenario> scenarioFromName(std::string_view name);

/// What a scenario wants the camera and clock to do, absent instructions.
struct ScenarioView {
    std::size_t focusIndex{0};
    double      frameSpan{0.0};      ///< metres across; 0 leaves the camera alone
    /// Index into kWarpLadder. A scenario knows its own timescale: planets want
    /// days per second, a low orbit wants minutes, a ten-minute burn wants
    /// close to real time.
    std::size_t warpIndex{5};
};

/// A satellite in a 400 km, 51.6 deg orbit, phased by \p index so a fleet
/// spreads around the orbit and across planes instead of stacking into one
/// pixel. The ISS inclination, because an equatorial orbit seen at 30 degrees
/// of camera tilt looks like a flat line.
[[nodiscard]] LaunchRequest makeLaunchRequest(BodyId around, int index);

/// Populate the world and say where to point.
ScenarioView applyScenario(World& world, Scenario scenario);

}  // namespace omma::app
