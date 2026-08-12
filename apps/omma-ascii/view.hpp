// View-side state: what the user is looking at, and the discrete warp/zoom
// ladders they move along. Warp is a ladder of named timescales, not a free
// multiplier — you almost always want "one day per second", never "37194x".
#pragma once

#include "core/vec3.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace omma::app {

struct WarpStep { double factor; const char* label; };

inline constexpr std::array<WarpStep, 12> kWarpLadder{{
    {1.0,           "real time"},
    {60.0,          "1 min/s"},
    {600.0,         "10 min/s"},
    {3'600.0,       "1 hour/s"},
    {21'600.0,      "6 hours/s"},
    {86'400.0,      "1 day/s"},
    {604'800.0,     "1 week/s"},
    {2'629'800.0,   "1 month/s"},
    {7'889'400.0,   "3 months/s"},
    {31'557'600.0,  "1 year/s"},
    {157'788'000.0, "5 years/s"},
    {631'152'000.0, "20 years/s"},
}};

struct ZoomPreset { double metresAcross; const char* label; };

inline constexpr std::array<ZoomPreset, 6> kZoomPresets{{
    {3.0e7,  "low orbit"},      // a 400 km orbit fills about a quarter of the view
    {1.2e9,  "Earth-Moon"},
    {3.6e11, "inner system"},
    {1.7e12, "to Jupiter"},
    {9.5e12, "to Neptune"},
    {1.6e13, "everything"},
}};

/// Framing used when something is launched, matching zoom preset 1.
inline constexpr double kLowOrbitSpan = 3.0e7;

/// Integration budget in CRAFT-steps per frame, shared across the fleet.
///
/// A fixed steps-per-frame cap holds the frame rate for one craft and
/// collapses it for twelve (measured: 7 fps). Dividing by fleet size keeps a
/// frame near budget whatever is in orbit; the honest consequence is that
/// simulated time falls behind sooner with more craft, and the HUD says so.
inline constexpr std::int64_t kIntegrationCraftStepBudget = 6'000;

/// Where the milliseconds in a frame went. Press 'f' to see it. Stays in
/// because "why is this not 30 fps" comes back every time the renderer grows,
/// and four timestamps settle what speculation cannot.
struct FrameTimings {
    double drawMs{0.0};
    double presentMs{0.0};
    double writeMs{0.0};
    double sleepMs{0.0};
    double totalMs{0.0};
    std::size_t frameBytes{0};
};

struct ViewState {
    std::size_t warpIndex{5};        // 1 day per second
    std::size_t focusIndex{0};       // the Sun
    bool        paused{false};
    bool        showOrbits{true};
    bool        showLabels{true};
    bool        showHelp{false};
    bool        showTimings{false};
    bool        running{true};
    Vec3        panOffset{};
};

// One focus index over bodies THEN spacecraft, so `tab` walks the whole scene
// and the camera-follow code does not care which kind of thing it is watching.

inline std::size_t focusTargetCount(const World& world) {
    return world.system().size() + world.spacecraft().size();
}

inline const Spacecraft* focusedCraft(const World& world, const ViewState& view) {
    const std::size_t bodies = world.system().size();
    if (view.focusIndex < bodies) {
        return nullptr;
    }
    const std::size_t craftIndex = view.focusIndex - bodies;
    return craftIndex < world.spacecraft().size() ? &world.spacecraft()[craftIndex]
                                                  : nullptr;
}

/// Where the camera should look.
///
/// Following a spacecraft centres on the body it ORBITS, not on the craft:
/// centring the craft swings the world around it every 90 minutes and pushes
/// the far half of its own orbit off screen — exactly the part you are
/// watching while burning. The orbit is the subject, not the vehicle.
inline Vec3 focusPosition(const World& world, const ViewState& view) {
    const std::size_t bodies = world.system().size();
    if (view.focusIndex < bodies) {
        return world.system().bodies()[view.focusIndex]->sample(world.now()).position;
    }
    if (const Spacecraft* craft = focusedCraft(world, view); craft != nullptr) {
        return world.system().bodies()[craft->centralBodyIndex]
            ->sample(world.now())
            .position;
    }
    return Vec3::zero();
}

/// Steps per frame the integrator can afford, given what is in orbit. With no
/// spacecraft every body is analytic and a step is one integer addition, so
/// warp is effectively unbounded.
inline std::int64_t stepBudgetFor(const World& world) {
    const std::size_t craft = world.spacecraft().size();
    if (craft == 0) {
        return 1'000'000'000'000LL;
    }
    return std::max<std::int64_t>(
        1, kIntegrationCraftStepBudget / static_cast<std::int64_t>(craft));
}

}  // namespace omma::app
