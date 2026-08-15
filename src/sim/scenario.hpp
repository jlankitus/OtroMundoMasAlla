// Scenarios: named, fully deterministic starting states. Sim-level, because
// every client wants them — the terminal app, the engine FFI, the QA
// harnesses — and a scenario name plus a step count must mean the same world
// everywhere. Clients keep their own presentation hints (camera, warp).
// See docs/DESIGN.md §8.
#pragma once

#include "sim/world.hpp"

#include <optional>
#include <string_view>

namespace omma {

enum class Scenario { Empty, Leo, Constellation, Transfer };

[[nodiscard]] std::optional<Scenario> scenarioFromName(std::string_view name);

/// A satellite in a 400 km, 51.6 deg orbit, phased by \p index so a fleet
/// spreads around the orbit and across planes instead of stacking into one
/// pixel. The ISS inclination, because an equatorial orbit reads as a flat
/// line on a tilted view.
[[nodiscard]] LaunchRequest makeLaunchRequest(BodyId around, int index);

/// Populate \p world with the scenario's spacecraft and commands.
void applyScenario(World& world, Scenario scenario);

}  // namespace omma
