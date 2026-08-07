// ─────────────────────────────────────────────────────────────────────────────
// omma-headless — the graphics-free simulation driver.
//
// Right now it does nothing but prove the build works end to end. It grows
// into the scenario runner: load a scenario, step the world at a fixed dt,
// stream telemetry to disk, return non-zero if a mission constraint is
// violated. Keeping it alive from commit one means we never accidentally
// build a simulator that only runs inside a renderer.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/version.hpp"

#include <iostream>

int main() {
    std::cout << omma::versionBanner() << '\n'
              << "no scenario loaded; nothing to simulate yet\n";
    return 0;
}
