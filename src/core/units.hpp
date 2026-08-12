// ─────────────────────────────────────────────────────────────────────────────
// Units and physical constants. THE RULE: SI everywhere internally — metres,
// kilograms, seconds, radians. Kilometres, days, degrees and AU exist only at
// the boundary (parsing, HUD) and convert the moment they cross in — the Mars
// Climate Orbiter failure mode. Literals rather than a dimensional-analysis
// library: SI as a documented invariant, with the conversion visible at the
// call site — `6371.0_km` cannot be misread; `6371000.0` can.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace omma::constants {

// ── Mathematical ─────────────────────────────────────────────────────────────
inline constexpr double kPi     = 3.14159265358979323846;
inline constexpr double kTwoPi  = 2.0 * kPi;
inline constexpr double kHalfPi = 0.5 * kPi;

// ── Physical ─────────────────────────────────────────────────────────────────

/// Newtonian constant of gravitation, m^3 kg^-1 s^-2 (CODATA 2018). The least
/// precisely known fundamental constant (~22 ppm); orbital mechanics uses the
/// measured GM products instead (kGm* in physics/solar_system.hpp). Kept only
/// for bodies with a known mass but no published GM.
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

/// Exactly 365.25 days by definition — astronomy's year, because it never varies.
inline constexpr double kSecondsPerJulianYear = 365.25 * kSecondsPerDay;

/// Julian Date of the J2000 epoch (2000-01-01 12:00:00 TT).
inline constexpr double kJulianDateJ2000 = 2'451'545.0;

}  // namespace omma::constants

namespace omma::literals {

// Two overloads each: C++ passes `1.5_km` as long double and `1500_km` as
// unsigned long long, and requiring a decimal point is a papercut.

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

// Time literals deliberately absent: use std::chrono_literals (1s, 90min, 24h),
// which give the exact integer durations core/epoch.hpp wants; ours would not.

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
