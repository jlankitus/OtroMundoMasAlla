// A deliberately trivial test. Its job is not to catch bugs in versionBanner();
// its job is to prove that the whole chain works — CMake configures, Catch2
// fetches and links, ctest discovers cases, and a failure turns CI red.
//
// Write this test on day one of every project. If the test harness is not
// green before there is anything to test, it will not be green later either.

#include "core/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("version banner reports the project name and version", "[core][version]") {
    const auto banner = omma::versionBanner();

    REQUIRE(banner.find(omma::kProjectName) != std::string_view::npos);
    REQUIRE(banner.find(omma::kVersion) != std::string_view::npos);
}

TEST_CASE("version components are consistent with the version string", "[core][version]") {
    // Guards against someone hand-editing the generated header instead of
    // bumping project(VERSION ...) in CMakeLists.txt.
    STATIC_REQUIRE(omma::kVersionMajor >= 0);
    STATIC_REQUIRE(omma::kVersionMinor >= 0);
    STATIC_REQUIRE(omma::kVersionPatch >= 0);

    REQUIRE(omma::kVersion.find(std::to_string(omma::kVersionMajor)) == 0);
}
