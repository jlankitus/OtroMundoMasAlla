# ─────────────────────────────────────────────────────────────────────────────
# Test suite.
#
# WHY CATCH2 AND NOT GOOGLETEST/DOCTEST
# Almost every assertion in this project is "is this floating-point number
# close enough?". Catch2 ships first-class matchers for exactly that question —
# WithinRel (relative error), WithinAbs (absolute error) and WithinULP (units
# in the last place) — which lets a test say what kind of closeness it means.
# "The orbit radius is within 1e-9 relative" and "the position is within 1 metre
# absolute" are different claims, and a physics test that conflates them is a
# test that will eventually lie to you.
#
# WHY ONE EXECUTABLE AND NOT ONE PER FILE
# Catch2's main is not cheap to compile. Building it once and linking every
# test translation unit into a single binary keeps the incremental build fast.
# catch_discover_tests still registers each TEST_CASE with CTest individually,
# so `ctest -R Kepler` works and failures are reported per-case.
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE
    SYSTEM                      # treat its headers as system => its warnings
)                               # are not our warnings
FetchContent_MakeAvailable(Catch2)

add_executable(omma_tests
    test_console.cpp
    test_ffi.cpp
    test_epoch.cpp
    test_ground_track.cpp
    test_integrator.cpp
    test_j2.cpp
    test_kepler.cpp
    test_orbital_elements.cpp
    test_render.cpp
    test_sim_clock.cpp
    test_solar_system.cpp
    test_vec3.cpp
    test_world.cpp
    test_version.cpp
)

target_link_libraries(omma_tests
    PRIVATE
        omma::core
        omma::physics
        omma::render
        omma::sim
        omma::ffi
        omma::compile_options
        Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(omma_tests)
