# ─────────────────────────────────────────────────────────────────────────────
# omma::physics — orbits, gravity, propagation. Knows about time and vectors;
# knows nothing about scheduling, telemetry or rendering.
# ─────────────────────────────────────────────────────────────────────────────

add_library(omma_physics
    gravity_field.cpp
    atmosphere.cpp
    ground_track.cpp
    integrator.cpp
    kepler_ephemeris.cpp
    orbital_elements.cpp
    solar_system.cpp
)
add_library(omma::physics ALIAS omma_physics)

# PUBLIC because physics headers expose core types (Epoch, Vec3) in their
# signatures, so anyone linking physics needs core's include path too. Getting
# PUBLIC/PRIVATE right here is what stops a project from degenerating into one
# target that everything links to everything else.
target_link_libraries(omma_physics
    PUBLIC  omma::core
    PRIVATE omma::compile_options
)
