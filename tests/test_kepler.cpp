// Tests for Kepler's equation, M = E - e*sin(E).
//
// This one function is the load-bearing beam of the entire analytic
// propagation path. Every planet position, every on-rails satellite, every
// trajectory preview goes through it. It gets tested harder than anything else
// in the codebase, and it gets tested against its own *definition* rather than
// against a table of expected outputs — because a residual check is a proof
// for every input, while a table is a spot check for the inputs someone
// happened to think of.

#include "core/units.hpp"
#include "physics/orbital_elements.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::solveKeplerEquation;
using omma::wrapToPi;
using omma::wrapToTwoPi;

TEST_CASE("the solution satisfies Kepler's equation by definition", "[physics][kepler]") {
    // The strongest possible test: for a dense sweep of the entire input
    // domain, substitute the answer back in and require the residual to
    // vanish. No expected-value table can match this for coverage.
    const double eccentricities[] = {0.0, 1e-6, 0.01, 0.1, 0.2056,   // Mercury
                                     0.5, 0.8, 0.9, 0.95, 0.99, 0.999};

    for (const double e : eccentricities) {
        for (int i = -360; i <= 360; ++i) {
            const double M = omma::toRadians(static_cast<double>(i));
            const double E = solveKeplerEquation(M, e);
            const double residual = wrapToPi((E - e * std::sin(E)) - M);

            INFO("e = " << e << ", M = " << i << " deg, E = " << E);
            REQUIRE_THAT(residual, WithinAbs(0.0, 1e-11));
        }
    }
}

TEST_CASE("a circular orbit has E == M exactly", "[physics][kepler]") {
    // e = 0 collapses the equation to E = M. Returning it without iterating is
    // both faster and exact, and circular orbits are the overwhelmingly common
    // case for constellations.
    for (int i = 0; i < 360; i += 7) {
        const double M = omma::toRadians(static_cast<double>(i));
        REQUIRE(solveKeplerEquation(M, 0.0) == wrapToPi(M));
    }
}

TEST_CASE("known landmark values", "[physics][kepler]") {
    // At M = 0 the body is at periapsis and at M = pi it is at apoapsis,
    // for every eccentricity. Those two are exact, so they make good anchors
    // against a solver that is subtly biased.
    for (const double e : {0.0, 0.1, 0.5, 0.9}) {
        REQUIRE_THAT(solveKeplerEquation(0.0, e), WithinAbs(0.0, 1e-13));
        REQUIRE_THAT(std::abs(solveKeplerEquation(omma::constants::kPi, e)),
                     WithinAbs(omma::constants::kPi, 1e-12));
    }
}

TEST_CASE("the solver is odd-symmetric about M = 0", "[physics][kepler]") {
    // E(-M) == -E(M). A solver whose initial guess is biased in one direction
    // breaks this before it breaks the residual check, so it catches a
    // different class of bug.
    for (const double e : {0.1, 0.6, 0.95}) {
        for (int i = 1; i < 180; i += 11) {
            const double M = omma::toRadians(static_cast<double>(i));
            REQUIRE_THAT(solveKeplerEquation(-M, e),
                         WithinAbs(-solveKeplerEquation(M, e), 1e-11));
        }
    }
}

TEST_CASE("inputs outside [-pi, pi] are wrapped, not rejected", "[physics][kepler]") {
    // Mean anomaly grows without bound as a simulation runs; after a year in
    // low Earth orbit it is in the thousands of radians. Wrapping has to be
    // internal or every caller has to remember.
    const double e = 0.3;
    const double base = 1.0;
    for (int turns = -5; turns <= 5; ++turns) {
        const double shifted = base + static_cast<double>(turns) * omma::constants::kTwoPi;
        REQUIRE_THAT(solveKeplerEquation(shifted, e),
                     WithinAbs(solveKeplerEquation(base, e), 1e-11));
    }
}

TEST_CASE("high eccentricity near periapsis still converges", "[physics][kepler]") {
    // The hard case. At e = 0.999 the curve is nearly flat around periapsis,
    // Newton's derivative approaches 1 - e = 0.001, and a poor starting guess
    // can be thrown a long way or oscillate. This is the region where a naive
    // implementation quietly returns garbage instead of failing loudly.
    const double e = 0.999;
    for (int i = 0; i <= 100; ++i) {
        const double M = omma::toRadians(static_cast<double>(i) * 0.01);   // 0 to 1 degree
        const double E = solveKeplerEquation(M, e);
        REQUIRE(std::isfinite(E));
        REQUIRE_THAT(wrapToPi((E - e * std::sin(E)) - M), WithinAbs(0.0, 1e-11));
    }
}

TEST_CASE("eccentric and true anomaly are mutual inverses", "[physics][kepler]") {
    for (const double e : {0.0, 0.05, 0.4, 0.85, 0.97}) {
        for (int i = -170; i <= 170; i += 13) {
            const double nu = omma::toRadians(static_cast<double>(i));
            const double E = omma::eccentricAnomalyFromTrue(nu, e);
            const double back = omma::trueAnomalyFromEccentric(E, e);
            INFO("e = " << e << ", nu = " << i << " deg");
            REQUIRE_THAT(wrapToPi(back - nu), WithinAbs(0.0, 1e-12));
        }
    }
}

TEST_CASE("true anomaly leads eccentric anomaly on the outbound leg",
          "[physics][kepler]") {
    // Physical sanity: between periapsis and apoapsis the body is moving
    // slowly and covering angle faster than the uniform-rate helper angle, so
    // nu > E > M. Getting these three confused is the classic orbital
    // mechanics mistake, and this test states which is which.
    const double e = 0.6;
    const double M = omma::toRadians(45.0);
    const double E = solveKeplerEquation(M, e);
    const double nu = omma::trueAnomalyFromEccentric(E, e);

    REQUIRE(nu > E);
    REQUIRE(E > M);
}

TEST_CASE("angle wrapping helpers", "[physics][kepler]") {
    using omma::constants::kPi;
    using omma::constants::kTwoPi;

    REQUIRE_THAT(wrapToTwoPi(0.0), WithinAbs(0.0, 1e-15));
    REQUIRE_THAT(wrapToTwoPi(-0.5), WithinAbs(kTwoPi - 0.5, 1e-15));
    REQUIRE_THAT(wrapToTwoPi(kTwoPi + 0.5), WithinAbs(0.5, 1e-14));
    REQUIRE(wrapToTwoPi(-100.0) >= 0.0);
    REQUIRE(wrapToTwoPi(-100.0) < kTwoPi);

    REQUIRE_THAT(wrapToPi(0.0), WithinAbs(0.0, 1e-15));
    REQUIRE_THAT(wrapToPi(kTwoPi - 0.5), WithinAbs(-0.5, 1e-14));
    REQUIRE(wrapToPi(100.0) >= -kPi);
    REQUIRE(wrapToPi(100.0) < kPi);
}
