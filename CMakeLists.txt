cmake_minimum_required(VERSION 3.21)

project(OtroMundoMasAlla
    VERSION     0.4.0
    DESCRIPTION "A deterministic solar system and spacecraft simulator"
    LANGUAGES   CXX)

# ─────────────────────────────────────────────────────────────────────────────
# Guardrails
# ─────────────────────────────────────────────────────────────────────────────
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds pollute the tree and break .gitignore.\n"
        "Use an out-of-source build:  cmake -B build")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # plain -std=c++20, never -std=gnu++20

# Everything executable lands in one place, so the apps can find each other
# and so CI has a single directory to archive.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(OMMA_BUILD_TESTS       "Build the test suite"              ON)
option(OMMA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Shared compile settings
#
# Carried by an INTERFACE target rather than global CMAKE_CXX_FLAGS. Global
# flags leak into every dependency we ever FetchContent, and third-party code
# is not obliged to build clean under -Wconversion. An INTERFACE target applies
# only to targets that explicitly link it — our code, and nothing else.
# ─────────────────────────────────────────────────────────────────────────────
add_library(omma_compile_options INTERFACE)
add_library(omma::compile_options ALIAS omma_compile_options)

if(MSVC)
    target_compile_options(omma_compile_options INTERFACE
        /W4                 # high warning level
        /permissive-        # standards conformance
        /Zc:__cplusplus     # report the real __cplusplus value
        /utf-8
        /fp:precise         # DETERMINISM: no reassociation of float math
    )
    if(OMMA_WARNINGS_AS_ERRORS)
        target_compile_options(omma_compile_options INTERFACE /WX)
    endif()
else()
    target_compile_options(omma_compile_options INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow            # a shadowed variable in an integrator is a silent
                            # accuracy bug, not a style nit
        -Wconversion -Wsign-conversion
        -Wdouble-promotion  # catches accidental float->double in hot loops
        -ffp-contract=off   # DETERMINISM: no fused multiply-add
    )
    if(OMMA_WARNINGS_AS_ERRORS)
        target_compile_options(omma_compile_options INTERFACE -Werror)
    endif()
endif()

# NOTE ON DETERMINISM
# ───────────────────
# We never enable -ffast-math / /fp:fast. Those let the compiler reassociate
# floating-point expressions, which means (a + b) + c may become a + (b + c) —
# a different answer in IEEE-754. The result is a simulator that produces
# different trajectories on different compilers, or between debug and release.
# A simulation you cannot reproduce is a simulation you cannot debug.

add_subdirectory(src)
add_subdirectory(apps)

if(OMMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  compiler ......... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests ............ ${OMMA_BUILD_TESTS}")
message(STATUS "  warnings as errors ${OMMA_WARNINGS_AS_ERRORS}")
message(STATUS "")
