#include "core/units.hpp"
#include "core/vec3.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

using omma::Vec3;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Vec3 arithmetic behaves like a vector space", "[core][vec3]") {
    constexpr Vec3 a{1.0, 2.0, 3.0};
    constexpr Vec3 b{4.0, -5.0, 6.0};

    // constexpr, so these are checked by the compiler, not at run time.
    STATIC_REQUIRE(a + b == Vec3{5.0, -3.0, 9.0});
    STATIC_REQUIRE(a - b == Vec3{-3.0, 7.0, -3.0});
    STATIC_REQUIRE(a * 2.0 == Vec3{2.0, 4.0, 6.0});
    STATIC_REQUIRE(2.0 * a == a * 2.0);
    STATIC_REQUIRE(-a == Vec3{-1.0, -2.0, -3.0});
    STATIC_REQUIRE(a + Vec3::zero() == a);
}

TEST_CASE("dot product satisfies its defining identities", "[core][vec3]") {
    constexpr Vec3 a{1.0, 2.0, 3.0};
    constexpr Vec3 b{4.0, -5.0, 6.0};

    STATIC_REQUIRE(dot(a, b) == dot(b, a));            // symmetric
    STATIC_REQUIRE(dot(a, a) == a.normSquared());      // self-dot is length^2
    STATIC_REQUIRE(dot(Vec3::unitX(), Vec3::unitY()) == 0.0);
}

TEST_CASE("cross product satisfies its defining identities", "[core][vec3]") {
    constexpr Vec3 a{1.0, 2.0, 3.0};
    constexpr Vec3 b{4.0, -5.0, 6.0};

    STATIC_REQUIRE(cross(a, b) == -cross(b, a));       // anti-commutative
    STATIC_REQUIRE(cross(a, a) == Vec3::zero());
    STATIC_REQUIRE(cross(Vec3::unitX(), Vec3::unitY()) == Vec3::unitZ());

    // The result is perpendicular to both inputs. This one is the reason we
    // care: the specific angular momentum vector h = r x v defines the orbital
    // plane, and every element derivation leans on this being exactly true.
    REQUIRE_THAT(dot(cross(a, b), a), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(dot(cross(a, b), b), WithinAbs(0.0, 1e-12));
}

TEST_CASE("norm and normalized", "[core][vec3]") {
    const Vec3 v{3.0, 4.0, 0.0};
    REQUIRE_THAT(v.norm(), WithinRel(5.0, 1e-15));
    REQUIRE_THAT(v.normalized().norm(), WithinRel(1.0, 1e-15));

    SECTION("normalizing a zero vector yields zero, not NaN") {
        // A NaN here would propagate silently through the integrator and turn
        // one bad frame into a permanently dead run. Failing loudly is better,
        // but returning a defined "no direction" is better still.
        const Vec3 z = Vec3::zero().normalized();
        REQUIRE(z == Vec3::zero());
        REQUIRE(z.isFinite());
    }
}

TEST_CASE("isFinite detects NaN and infinity", "[core][vec3]") {
    REQUIRE(Vec3{1.0, 2.0, 3.0}.isFinite());

    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(Vec3{nan, 0.0, 0.0}.isFinite());
    REQUIRE_FALSE(Vec3{0.0, inf, 0.0}.isFinite());
    REQUIRE_FALSE(Vec3{0.0, 0.0, -inf}.isFinite());
}

TEST_CASE("angleBetween stays accurate for nearly-parallel vectors", "[core][vec3]") {
    using namespace omma::literals;

    REQUIRE_THAT(omma::angleBetween(Vec3::unitX(), Vec3::unitY()), WithinRel(90.0_deg, 1e-12));
    REQUIRE_THAT(omma::angleBetween(Vec3::unitX(), -Vec3::unitX()), WithinRel(180.0_deg, 1e-12));

    // The reason we use atan2(|a x b|, a.b) instead of acos(a.b/|a||b|).
    // For a tiny angle, acos' argument is ~1 and its derivative is unbounded,
    // so the textbook formula loses most of its significant digits exactly
    // where an orbit-vs-itself comparison lives.
    constexpr double tiny = 1e-9;
    const Vec3 a{1.0, 0.0, 0.0};
    const Vec3 b{1.0, tiny, 0.0};
    REQUIRE_THAT(omma::angleBetween(a, b), WithinRel(tiny, 1e-6));
}
