#include "core/epoch.hpp"

#include "core/units.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace omma {
namespace {

/// Days from 1970-01-01 to the given proleptic-Gregorian date. Howard
/// Hinnant's days_from_civil: published, proven, exact for the full range.
/// See: https://howardhinnant.github.io/date_algorithms.html
constexpr std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);                       // [0, 399]
    // Hinnant writes this as (m + (m > 2 ? -3 : 9)), which applies unary minus
    // to an unsigned. Same arithmetic, spelled so it does not warn.
    const unsigned mp = (m > 2) ? (m - 3u) : (m + 9u);                           // [0, 11]
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;                         // [0, 365]
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;               // [0, 146096]
    return era * 146'097 + static_cast<std::int64_t>(doe) - 719'468;
}

/// Inverse of daysFromCivil.
constexpr void civilFromDays(std::int64_t z, int& year, unsigned& month, unsigned& day) noexcept {
    z += 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = static_cast<unsigned long>(z - era * 146'097);              // [0, 146096]
    const unsigned long yoe =
        (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;                 // [0, 399]
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    const unsigned long mp = (5 * doy + 2) / 153;                                // [0, 11]
    day   = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);                 // [1, 31]
    month = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);                    // [1, 12]
    year  = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

/// Days from the Unix epoch (1970-01-01 00:00) to the J2000 *date* (2000-01-01).
/// The extra half-day to J2000's 12:00 noon is applied separately.
constexpr std::int64_t kUnixDaysToJ2000Date = daysFromCivil(2000, 1, 1);
static_assert(kUnixDaysToJ2000Date == 10'957);

constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
constexpr std::int64_t kSecondsPerDayI = 86'400;

}  // namespace

Duration fromSeconds(double seconds) noexcept {
    // llround rather than a cast: truncation toward zero would let a scenario
    // authored as "1.0 s" land a nanosecond short.
    return Duration{static_cast<std::int64_t>(std::llround(seconds * 1e9))};
}

Epoch Epoch::fromCivil(const CivilTime& civil) {
    if (civil.month < 1 || civil.month > 12 || civil.day < 1 || civil.day > 31) {
        throw std::out_of_range("Epoch::fromCivil: month or day out of range");
    }

    const std::int64_t days = daysFromCivil(civil.year, civil.month, civil.day)
                            - kUnixDaysToJ2000Date;

    // Whole seconds in integers; only the fractional second goes through a
    // double, so conversion error is bounded by 1 ns regardless of date.
    const auto wholeSeconds = static_cast<std::int64_t>(civil.second);
    const double fractional = civil.second - static_cast<double>(wholeSeconds);

    const std::int64_t secondsOfDay = static_cast<std::int64_t>(civil.hour) * 3600
                                    + static_cast<std::int64_t>(civil.minute) * 60
                                    + wholeSeconds;

    // J2000 is at 12:00, not 00:00, hence the half-day offset.
    const std::int64_t totalSeconds = days * kSecondsPerDayI + secondsOfDay - kSecondsPerDayI / 2;

    constexpr std::int64_t kMaxSeconds = std::numeric_limits<std::int64_t>::max() / kNanosPerSecond;
    if (totalSeconds > kMaxSeconds || totalSeconds < -kMaxSeconds) {
        throw std::out_of_range(
            "Epoch::fromCivil: date is outside the representable range "
            "(roughly 1708 to 2292)");
    }

    return Epoch{Duration{totalSeconds * kNanosPerSecond}
                 + fromSeconds(fractional)};
}

Epoch Epoch::fromJulianDate(double jd) noexcept {
    return Epoch::fromSecondsSinceJ2000(
        (jd - constants::kJulianDateJ2000) * constants::kSecondsPerDay);
}

double Epoch::daysSinceJ2000() const noexcept {
    return secondsSinceJ2000() / constants::kSecondsPerDay;
}

double Epoch::julianDate() const noexcept {
    return constants::kJulianDateJ2000 + daysSinceJ2000();
}

CivilTime Epoch::toCivil() const noexcept {
    // Shift back to a midnight-based, Unix-based count before splitting, so
    // that floor division does the right thing for pre-J2000 instants.
    const std::int64_t ns = sinceJ2000_.count();

    std::int64_t seconds = ns / kNanosPerSecond;
    std::int64_t subNanos = ns % kNanosPerSecond;
    if (subNanos < 0) {           // C++ truncates toward zero; we want floor.
        subNanos += kNanosPerSecond;
        seconds -= 1;
    }

    seconds += kSecondsPerDayI / 2;                    // J2000 noon -> midnight
    std::int64_t days = seconds / kSecondsPerDayI;
    std::int64_t secondsOfDay = seconds % kSecondsPerDayI;
    if (secondsOfDay < 0) {
        secondsOfDay += kSecondsPerDayI;
        days -= 1;
    }

    CivilTime out{};
    civilFromDays(days + kUnixDaysToJ2000Date, out.year, out.month, out.day);
    out.hour   = static_cast<unsigned>(secondsOfDay / 3600);
    out.minute = static_cast<unsigned>((secondsOfDay % 3600) / 60);
    out.second = static_cast<double>(secondsOfDay % 60)
               + static_cast<double>(subNanos) * 1e-9;
    return out;
}

std::string Epoch::toString() const {
    const CivilTime c = toCivil();
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02u:%02u:%06.3f",
                  c.year, c.month, c.day, c.hour, c.minute, c.second);
    return std::string{buffer};
}

}  // namespace omma
