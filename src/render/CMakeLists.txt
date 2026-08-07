# ─────────────────────────────────────────────────────────────────────────────
# omma::render — a character-grid canvas and a world-to-screen camera.
#
# A library rather than files inside apps/omma-ascii, for one reason: things
# that live inside an application binary do not get tested. Both of these have
# real logic in them -- the camera's cell-aspect correction and range clamping,
# the canvas's clipping and colour-run compression -- and that logic is exactly
# the kind that fails quietly and gets blamed on "the terminal being weird".
#
# It depends on core for Vec3 and nothing else. It has never heard of an orbit.
# ─────────────────────────────────────────────────────────────────────────────

add_library(omma_render
    camera.cpp
    canvas.cpp
)
add_library(omma::render ALIAS omma_render)

target_link_libraries(omma_render
    PUBLIC  omma::core
    PRIVATE omma::compile_options
)
