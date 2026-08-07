// ─────────────────────────────────────────────────────────────────────────────
// Epoch — an instant in simulated time.
//
// Stored as int64 nanoseconds since J2000 (2000-01-01 12:00:00 TT), which is
// the reference instant essentially all modern astrodynamics uses.
//
// WHY INTEGER NANOSECONDS AND NOT DOUBLE SECONDS
// Because simulated time is *accumulated*, and floating point accumulation
// drifts. Adding 1e-3 to a double a million times does not give you 1000.0; it
// gives you 1000.0000000000021, and the error depends on the current magnitude
// of the accumulator, so it changes as the run goes on. Integers are exact:
// a million additions of 1'000'000 ns is precisely 1'000'000'000'000 ns, on
// every machine, forever.
//
// Physics wants doubles, of course. It gets them via secondsSinceJ2000() —
// converted at the point of use, never accumulated.
//
// RANGE
// int64 nanoseconds spans about ±292 years around J2000, i.e. 1708 to 2292.
// Far more than this simulator needs, and the limit is checked rather than
// assumed.
//
// KNOWN SIMPLIFICATION
// We treat the civil-calendar helpers as if UTC and TT were the same timescale.
// They are not: TT ran 69.184 s ahead of UTC as of 2026, and UTC additionally
// inserts leap seconds. Modelling that properly needs a leap-second table that
// must be updated by hand as the IERS announces them. For a simulator whose
// job is orbital dynamics rather than timekeeping, a fixed ~69 s offset is
// below every error that actually matters here. It is recorded because an
// unwritten simplification becomes an unknown bug.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>

namespace omma {

/// A span of simulated time. Exact integer nanoseconds.
///
/// Aliased to std::chrono::nanoseconds so that std::chrono_literals work
/// directly: `1s`, `90min`, `24h` are all Durations with no conversion loss.
using Duration = std::chrono::nanoseconds;

/// Convert a Duration to seconds as a double. Use at the point the physics
/// needs a number, never to store time.
[[nodiscard]] constexpr double toSeconds(Duration d) noexcept {
    return static_cast<double>(d.count()) * 1e-9;
}

/// Convert seconds to a Duration, rounding to the nearest nanosecond.
[[nodiscard]] Duration fromSeconds(double seconds) noexcept;

/// A calendar breakdown, for display and for parsing scenario files. Never for
/// arithmetic — that is what Epoch and Duration are for.
struct CivilTime {
    int      year{2000};
    unsigned month{1};    // 1-12
    unsigned day{1};      // 1-31
    unsigned hour{0};     // 0-23
    unsigned minute{0};   // 0-59
    double   second{0.0}; // [0, 60)

    friend bool operator==(const CivilTime&, const CivilTime&) = default;
};

class Epoch {
public:
    /// Default-constructs to J2000 exactly.
    constexpr Epoch() noexcept = default;

    [[nodiscard]] static constexpr Epoch j2000() noexcept { return Epoch{}; }

    [[nodiscard]] static constexpr Epoch fromNanosSinceJ2000(std::int64_t ns) noexcept {
        return Epoch{Duration{ns}};
    }

    [[nodiscard]] static Epoch fromSecondsSinceJ2000(double seconds) noexcept {
        return Epoch{fromSeconds(seconds)};
    }

    /// Build from a civil date. See the timescale simplification noted above.
    /// Throws std::out_of_range if the result would not fit in int64 nanos.
    [[nodiscard]] static Epoch fromCivil(const CivilTime& civil);

    /// Julian Date, the astronomer's continuous day count. JD 2451545.0 is
    /// J2000 by definition.
    [[nodiscard]] static Epoch fromJulianDate(double jd) noexcept;

    [[nodiscard]] constexpr Duration sinceJ2000() const noexcept { return sinceJ2000_; }
    [[nodiscard]] constexpr double secondsSinceJ2000() const noexcept { return toSeconds(sinceJ2000_); }
    [[nodiscard]] double daysSinceJ2000() const noexcept;
    [[nodiscard]] double julianDate() const noexcept;

    [[nodiscard]] CivilTime toCivil() const noexcept;

    /// "2026-08-06 14:30:00.000" — for HUDs and log lines.
    [[nodiscard]] std::string toString() const;

    constexpr Epoch& operator+=(Duration d) noexcept { sinceJ2000_ += d; return *this; }
    constexpr Epoch& operator-=(Duration d) noexcept { sinceJ2000_ -= d; return *this; }

    friend constexpr Epoch operator+(Epoch e, Duration d) noexcept { return e += d; }
    friend constexpr Epoch operator+(Duration d, Epoch e) noexcept { return e += d; }
    friend constexpr Epoch operator-(Epoch e, Duration d) noexcept { return e -= d; }

    /// Epoch minus Epoch is a Duration — the one subtraction that type-checks.
    /// This is the whole point of a distinct time-point type: you cannot
    /// accidentally add two absolute instants together and get nonsense.
    friend constexpr Duration operator-(Epoch a, Epoch b) noexcept {
        return a.sinceJ2000_ - b.sinceJ2000_;
    }

    friend constexpr auto operator<=>(Epoch, Epoch) noexcept = default;
    friend constexpr bool operator==(Epoch, Epoch) noexcept  = default;

private:
    explicit constexpr Epoch(Duration d) noexcept : sinceJ2000_{d} {}

    Duration sinceJ2000_{};
};

}  // namespace omma
