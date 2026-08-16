// Command-line options. --snapshot and --record exist so the renderer can be
// verified without a human watching it; see docs/QA.md for the harnesses that
// drive them.
#pragma once

#include "core/epoch.hpp"
#include "sim/scenario.hpp"
#include "render/canvas.hpp"

#include <cstddef>
#include <string>

namespace omma::app {

struct Options {
    bool                snapshot{false};
    render::ColourDepth depth{render::ColourDepth::TrueColour};
    bool                depthForced{false};
    render::BlockStyle  blocks{render::BlockStyle::HalfBlocks};
    bool                blocksForced{false};
    int                 columns{0};
    int                 rows{0};
    std::size_t         zoomPreset{4};   // "to Neptune"; see kZoomPresets
    std::string         focus{"Sun"};
    bool                focusForced{false};
    bool                zoomForced{false};
    CivilTime           date{2026, 8, 6, 12, 0, 0.0};
    double              elevationDeg{30.0};
    double              azimuthDeg{0.0};
    bool                showHelpAndExit{false};
    /// Open with the ground-track map up, so the QA harness can reach it.
    bool                showMap{false};

    Scenario            scenario{Scenario::Leo};
    /// Simulated steps before the first frame, so a scenario's EVOLVED state
    /// (the transfer burn takes 628 s) is reachable from --snapshot and
    /// therefore checkable by the QA harness.
    int                 settleSteps{0};
    /// Extra satellites before the first frame, for the same reason: a feature
    /// only reachable interactively is a feature the harness cannot check.
    int                 preLaunch{0};

    // Record mode: the live loop, headless and deterministic. The wall clock
    // becomes a fixed synthetic delta and the keyboard one scripted character
    // per frame — software-in-the-loop, applied to a renderer.
    int                 recordFrames{0};
    std::string         recordDir{"frames"};
    double              frameDeltaSeconds{1.0 / 30.0};
    std::string         keys;             ///< one per frame; '.' none, '@' prograde
    bool                startPaused{false};
};

void printUsage();

/// Fill \p out from argv. Returns false (after an stderr complaint) on bad input.
bool parseOptions(int argc, char** argv, Options& out);

}  // namespace omma::app
