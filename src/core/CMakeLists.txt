# ─────────────────────────────────────────────────────────────────────────────
# omma::core — units, math primitives, time. No orbits, no I/O, no rendering.
# ─────────────────────────────────────────────────────────────────────────────

# version.hpp is generated so the version number lives in exactly one place
# (the project() call) instead of being duplicated into a hand-edited header
# that inevitably drifts.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/core/version.hpp
    @ONLY)

add_library(omma_core
    version.cpp
)
add_library(omma::core ALIAS omma_core)

target_include_directories(omma_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src           # so headers read as "core/foo.hpp"
        ${CMAKE_BINARY_DIR}/generated       # ...including the generated ones
)

target_link_libraries(omma_core PRIVATE omma::compile_options)
