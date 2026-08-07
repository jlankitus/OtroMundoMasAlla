// ─────────────────────────────────────────────────────────────────────────────
// Units and physical constants.
//
// THE RULE: SI EVERYWHERE INTERNALLY.
// Metres, kilograms, seconds, radians. No exceptions inside core, physics or
// sim. Kilometres, days, degrees and AU exist only at the boundary — parsing a
// scenario file, printing a HUD — and are converted the moment they cross in.
//
// The Mars Climate Orbiter burned up in 1999 because one team's software
// produced pound-force-seconds and another's consumed newton-seconds. The
// interface documentation said which was expected. Nobody checked.
//
// WHY LITERALS AND NOT A FULL DIMENSIONAL-ANALYSIS LIBRARY
// A templated Quantity<mass, length, time> makes unit errors a compile error,
// which is strictly stronger than what we do here. It also makes every physics
// expression twice as long to read and produces error messages that need their
// own glossary. For a codebase this size the readable compromise is: SI as a
// documented invariant, plus literals so the conversion is visible at the call
// site. `6371.0_km` cannot be misread; `6371000.0` can.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace omma::constants {

// ── Mathematical ─────────────────────────────────────────────────────────────
inline constexpr double kPi     = 3.14159265358979323846;
inline constexpr double kTwoPi  = 2.0 * kPi;
inline constexpr double kHalfPi = 0.5 * kPi;

// ── Physical ─────────────────────────────────────────────────────────────────

/// Newtonian constant of gravitation, m^3 kg^-1 s^-2 (CODATA 2018).
///
/// Note this is the *least* precisely known fundamental constant — about 22
/// parts per million. Which is why orbital mechanics never uses it directly:
/// the product GM is measured by watching things orbit, to eleven or more
/// digits. See kGm* in physics/solar_system.hpp. We keep G here only for the
/// rare case of a body whose mass we know but whose GM nobody has published.
inline constexpr double kGravitationalConstant = 6.67430e-11;

/// Speed of light in vacuum, m/s. Exact by definition of the metre.
inline constexpr double kSpeedOfLight = 299'792'458.0;

// ── Length ───────────────────────────────────────────────────────────────────

/// Astronomical unit, in metres. Exact by IAU definition since 2012 — it is a
/// defined conversion factor now, not a measurement of Earth's orbit.
inline constexpr double kAu = 149'597'870'700.0;

// ── Time ─────────────────────────────────────────────────────────────────────
inline constexpr double kSecondsPerMinute = 60.0;
inline constexpr double kSecondsPerHour   = 3600.0;
inline constexpr double kSecondsPerDay    = 86'400.0;

/// A Julian year is exactly 365.25 days by definition. It is the year used in
/// astronomy precisely because it never varies, unlike the calendar year.
inline constexpr double kSecondsPerJulianYear = 365.25 * kSecondsPerDay;

/// Julian Date of the J2000 epoch (2000-01-01 12:00:00 TT).
inline constexpr double kJulianDateJ2000 = 2'451'545.0;

}  // namespace omma::constants

namespace omma::literals {

// Each literal has two overloads because C++ passes `1.5_km` as long double
// and `1500_km` as unsigned long long, and requiring a decimal point on every
// distance is the kind of papercut that makes people stop using the literal.

// ── Length → metres ──────────────────────────────────────────────────────────
[[nodiscard]] constexpr double operator""_m (long double v) noexcept { return static_cast<double>(v); }
[[nodiscard]] constexpr double operator""_m (unsigned long long v) noexcept { return static_cast<double>(v); }

[[nodiscard]] constexpr double operator""_km(long double v) noexcept { return static_cast<double>(v) * 1000.0; }
[[nodiscard]] constexpr double operator""_km(unsigned long long v) noexcept { return static_cast<double>(v) * 1000.0; }

[[nodiscard]] constexpr double operator""_au(long double v) noexcept { return static_cast<double>(v) * constants::kAu; }
[[nodiscard]] constexpr double operator""_au(unsigned long long v) noexcept { return static_cast<double>(v) * constants::kAu; }

// ── Angle → radians ──────────────────────────────────────────────────────────
[[nodiscard]] constexpr double operator""_rad(long double v) noexcept { return static_cast<double>(v); }
[[nodiscard]] constexpr double operator""_rad(unsigned long long v) noexcept { return static_cast<double>(v); }

[[nodiscard]] constexpr double operator""_deg(long double v) noexcept {
    return static_cast<double>(v) * constants::kPi / 180.0;
}
[[nodiscard]] constexpr double operator""_deg(unsigned long long v) noexcept {
    return static_cast<double>(v) * constants::kPi / 180.0;
}

// ── Speed → metres per second ────────────────────────────────────────────────
[[nodiscard]] constexpr double operator""_mps (long double v) noexcept { return static_cast<double>(v); }
[[nodiscard]] constexpr double operator""_mps (unsigned long long v) noexcept { return static_cast<double>(v); }

[[nodiscard]] constexpr double operator""_kmps(long double v) noexcept { return static_cast<double>(v) * 1000.0; }
[[nodiscard]] constexpr double operator""_kmps(unsigned long long v) noexcept { return static_cast<double>(v) * 1000.0; }

// Time literals deliberately absent: use std::chrono_literals (1s, 90min, 24h).
// Reimplementing them would give us a double where chrono gives us an exact
// integer duration, which is the opposite of what core/epoch.hpp wants.

}  // namespace omma::literals

namespace omma {

/// Radians → degrees, for display only.
[[nodiscard]] constexpr double toDegrees(double radians) noexcept {
    return radians * 180.0 / constants::kPi;
}

/// Degrees → radians, for parsing only.
[[nodiscard]] constexpr double toRadians(double degrees) noexcept {
    return degrees * constants::kPi / 180.0;
}

}  // namespace omma
