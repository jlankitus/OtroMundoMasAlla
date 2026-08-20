# ─────────────────────────────────────────────────────────────────────────────
# omma::sim — the world, one deterministic tick at a time. Owns celestial bodies,
# spacecraft and the clock. Knows nothing about pixels.
# ─────────────────────────────────────────────────────────────────────────────

add_library(omma_sim
    ascent.cpp
    scenario.cpp
    spacecraft.cpp
    world.cpp
)
add_library(omma::sim ALIAS omma_sim)

target_link_libraries(omma_sim
    PUBLIC  omma::physics
    PRIVATE omma::compile_options
)
