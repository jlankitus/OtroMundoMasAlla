// Frame composition: background, orbits, bodies, spacecraft, HUD.
#pragma once

#include "view.hpp"

#include "render/camera.hpp"
#include "render/canvas.hpp"
#include "sim/world.hpp"

namespace omma::app {

/// Compose one full frame into \p canvas, moving the camera to the focus.
void render(render::Canvas& canvas, render::Camera& camera, const World& world,
            const ViewState& view, double framesPerSecond, bool clamped,
            const FrameTimings& timings);

}  // namespace omma::app
