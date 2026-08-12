// ─────────────────────────────────────────────────────────────────────────────
// Epoch — an instant in simulated time, stored as int64 nanoseconds since
// J2000 (2000-01-01 12:00:00 TT). Integer, not double: accumulated floating
// point drifts; integer addition is exact on every machine (docs/DESIGN.md §3).
// Physics gets doubles via secondsSinceJ2000() — converted at the point of
// use, never accumulated. Range: about ±292 years around J2000 (1708 to 2292),
// checked rather than assumed. KNOWN SIMPLIFICATION: the civil-calendar
// helpers treat UTC and TT as the same timescale (really ~69 s apart, plus
// leap seconds) — below every error that matters for orbital dynamics.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>

namespace omma {

/// A span of simulated time. Exact integer nanoseconds; aliased to
/// std::chrono::nanoseconds so `1s`, `90min`, `24h` work with no loss.
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

    /// Epoch minus Epoch is a Duration — the one subtraction that type-checks;
    /// a distinct time-point type means two instants cannot be added by accident.
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
