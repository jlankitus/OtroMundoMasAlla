#include "core/epoch.hpp"
#include "core/units.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <type_traits>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::CivilTime;
using omma::Duration;
using omma::Epoch;

TEST_CASE("J2000 is the zero point", "[core][epoch]") {
    STATIC_REQUIRE(Epoch{}.sinceJ2000() == Duration{0});
    STATIC_REQUIRE(Epoch::j2000() == Epoch{});

    // J2000 is defined as Julian Date 2451545.0.
    REQUIRE_THAT(Epoch::j2000().julianDate(), WithinAbs(2'451'545.0, 1e-9));
}

TEST_CASE("civil calendar round-trips exactly", "[core][epoch]") {
    // J2000 is 2000-01-01 at *noon*, not midnight. Getting this half-day wrong
    // is the single most common bug in hand-rolled astronomical time code, so
    // it gets its own assertion.
    REQUIRE(Epoch::fromCivil(CivilTime{2000, 1, 1, 12, 0, 0.0}) == Epoch::j2000());
    REQUIRE(Epoch::fromCivil(CivilTime{2000, 1, 1, 0, 0, 0.0})
            == Epoch::j2000() - std::chrono::hours{12});

    const auto cases = {
        CivilTime{1970, 1, 1, 0, 0, 0.0},
        CivilTime{1999, 12, 31, 23, 59, 59.0},
        CivilTime{2000, 2, 29, 6, 30, 0.0},     // leap year: divisible by 400
        CivilTime{1900, 3, 1, 0, 0, 0.0},       // NOT a leap year: divisible by 100
        CivilTime{2024, 2, 29, 18, 45, 30.5},
        CivilTime{2026, 8, 6, 14, 30, 0.0},
        CivilTime{2100, 12, 31, 23, 59, 59.999},
    };

    for (const auto& c : cases) {
        const CivilTime back = Epoch::fromCivil(c).toCivil();
        INFO(c.year << "-" << c.month << "-" << c.day);
        REQUIRE(back.year == c.year);
        REQUIRE(back.month == c.month);
        REQUIRE(back.day == c.day);
        REQUIRE(back.hour == c.hour);
        REQUIRE(back.minute == c.minute);
        REQUIRE_THAT(back.second, WithinAbs(c.second, 1e-6));
    }
}

TEST_CASE("Julian Date matches published values", "[core][epoch]") {
    // Cross-checked against the standard JD conversion: 2026-08-06 00:00 is
    // 9713.5 days after J2000, so JD 2461258.5. An independent reference value
    // is worth more than a round-trip test, because a round trip is equally
    // happy with two mistakes that cancel.
    const Epoch e = Epoch::fromCivil(CivilTime{2026, 8, 6, 0, 0, 0.0});
    REQUIRE_THAT(e.julianDate(), WithinAbs(2'461'258.5, 1e-6));
    REQUIRE_THAT(e.daysSinceJ2000(), WithinAbs(9713.5, 1e-9));

    SECTION("and round-trips through fromJulianDate") {
        REQUIRE_THAT(Epoch::fromJulianDate(2'461'258.5).daysSinceJ2000(),
                     WithinAbs(9713.5, 1e-6));
    }
}

TEST_CASE("Epoch arithmetic is exact over long spans", "[core][epoch]") {
    // The headline property. One million additions of 100 ms must land on
    // exactly 100000 seconds, with no accumulated error at all.
    //
    // The same loop in double-precision seconds does not, and the comparison
    // is run right here so the claim is measured rather than believed.
    constexpr int kIterations = 1'000'000;
    constexpr auto kStep = 100ms;

    Epoch exact = Epoch::j2000();
    double naive = 0.0;
    for (int i = 0; i < kIterations; ++i) {
        exact += kStep;
        naive += 0.1;
    }

    const double expectedSeconds = 100'000.0;

    // Integer nanoseconds: not "close to", but equal.
    REQUIRE(exact.sinceJ2000() == std::chrono::seconds{100'000});
    REQUIRE(exact.secondsSinceJ2000() == expectedSeconds);

    // Floating point: demonstrably not equal. If this assertion ever starts
    // failing it means the naive approach got exact, which would be lovely and
    // would also mean the compiler is doing something we should know about.
    REQUIRE(naive != expectedSeconds);
    INFO("naive double accumulator drifted by " << (naive - expectedSeconds) << " s");
    REQUIRE(std::abs(naive - expectedSeconds) > 1e-9);
}

TEST_CASE("Epoch minus Epoch is a Duration", "[core][epoch]") {
    const Epoch a = Epoch::fromCivil(CivilTime{2026, 1, 1, 0, 0, 0.0});
    const Epoch b = a + 24h;

    STATIC_REQUIRE(std::is_same_v<decltype(b - a), Duration>);
    REQUIRE(b - a == 24h);
    REQUIRE(a - b == -24h);

    // Ordering comes free from the defaulted <=>.
    REQUIRE(a < b);
    REQUIRE(b > a);
    REQUIRE(a == a);
}

TEST_CASE("toString formats a readable timestamp", "[core][epoch]") {
    REQUIRE(Epoch::fromCivil(CivilTime{2026, 8, 6, 14, 30, 0.0}).toString()
            == "2026-08-06 14:30:00.000");
    REQUIRE(Epoch::j2000().toString() == "2000-01-01 12:00:00.000");
}

TEST_CASE("dates beyond the representable range are rejected", "[core][epoch]") {
    // int64 nanoseconds covers roughly 1708..2292. Outside that we throw
    // rather than silently wrap, because a wrapped epoch produces a plausible
    // looking simulation of entirely the wrong century.
    REQUIRE_THROWS_AS(Epoch::fromCivil(CivilTime{3000, 1, 1, 0, 0, 0.0}), std::out_of_range);
    REQUIRE_THROWS_AS(Epoch::fromCivil(CivilTime{1500, 1, 1, 0, 0, 0.0}), std::out_of_range);
    REQUIRE_THROWS_AS(Epoch::fromCivil(CivilTime{2026, 13, 1, 0, 0, 0.0}), std::out_of_range);
}
