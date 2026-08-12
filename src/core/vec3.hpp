// ─────────────────────────────────────────────────────────────────────────────
// Vec3 — a three-component vector of doubles. Double, not float: at Neptune's
// ~4.5e12 m a float resolves ~500 km, a double sub-millimetre. Rendering may
// drop to float at the very last step; physics never does. A plain struct on
// purpose — trivially copyable, no virtuals — anything fancier is paid for a
// billion times over in the integrator.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cmath>

namespace omma {

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    [[nodiscard]] static constexpr Vec3 zero() noexcept { return {}; }
    [[nodiscard]] static constexpr Vec3 unitX() noexcept { return {1.0, 0.0, 0.0}; }
    [[nodiscard]] static constexpr Vec3 unitY() noexcept { return {0.0, 1.0, 0.0}; }
    [[nodiscard]] static constexpr Vec3 unitZ() noexcept { return {0.0, 0.0, 1.0}; }

    constexpr Vec3& operator+=(const Vec3& r) noexcept { x += r.x; y += r.y; z += r.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& r) noexcept { x -= r.x; y -= r.y; z -= r.z; return *this; }
    constexpr Vec3& operator*=(double s)      noexcept { x *= s;   y *= s;   z *= s;   return *this; }
    constexpr Vec3& operator/=(double s)      noexcept { x /= s;   y /= s;   z /= s;   return *this; }

    /// Squared magnitude. Prefer this to norm() for comparisons — it avoids a
    /// square root, and "is A closer than B" never needed one.
    [[nodiscard]] constexpr double normSquared() const noexcept { return x * x + y * y + z * z; }

    [[nodiscard]] double norm() const noexcept { return std::sqrt(normSquared()); }

    /// Unit vector in the same direction. Returns ZERO for a zero-length input,
    /// not NaN: a zero vector means "no direction", and NaN silently poisons
    /// the whole run.
    [[nodiscard]] Vec3 normalized() const noexcept {
        const double n = norm();
        return n > 0.0 ? Vec3{x / n, y / n, z / n} : Vec3::zero();
    }

    [[nodiscard]] constexpr bool isFinite() const noexcept {
        // Not std::isfinite, which is not constexpr here: NaN != itself, and
        // the subtraction catches infinities.
        return (x == x) && (y == y) && (z == z)
            && (x - x == 0.0) && (y - y == 0.0) && (z - z == 0.0);
    }

    friend constexpr bool operator==(const Vec3&, const Vec3&) noexcept = default;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, const Vec3& b) noexcept { return a += b; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, const Vec3& b) noexcept { return a -= b; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 v, double s)      noexcept { return v *= s; }
[[nodiscard]] constexpr Vec3 operator*(double s, Vec3 v)      noexcept { return v *= s; }
[[nodiscard]] constexpr Vec3 operator/(Vec3 v, double s)      noexcept { return v /= s; }
[[nodiscard]] constexpr Vec3 operator-(const Vec3& v)         noexcept { return {-v.x, -v.y, -v.z}; }

[[nodiscard]] constexpr double dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr double distanceSquared(const Vec3& a, const Vec3& b) noexcept {
    return (a - b).normSquared();
}

[[nodiscard]] inline double distance(const Vec3& a, const Vec3& b) noexcept {
    return (a - b).norm();
}

/// Angle between two vectors, in radians, in [0, pi]. Uses atan2(|a x b|, a.b)
/// rather than the textbook acos form, which loses catastrophic precision for
/// nearly-parallel vectors — exactly the orbit-vs-itself-one-step-later case.
[[nodiscard]] inline double angleBetween(const Vec3& a, const Vec3& b) noexcept {
    return std::atan2(cross(a, b).norm(), dot(a, b));
}

}  // namespace omma
