// ─────────────────────────────────────────────────────────────────────────────
// Orbital elements, Kepler's equation, and the conversions between elements
// and state vectors.
//
// WHY TWO REPRESENTATIONS OF THE SAME THING
// A state vector (r, v) tells you where a body is right now. Orbital elements
// tell you what its orbit *is*. They carry identical information, but five of
// the six elements are constant for an unperturbed orbit while all six
// components of the state vector change every instant.
//
// That is the whole trick. "Where is Jupiter in 400 years?" is an integration
// problem in state-vector form and a single evaluation in element form.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/state_vector.hpp"

namespace omma {

/// The classical (Keplerian) orbital elements.
///
/// Angles are radians, distances metres, consistently with the SI-internally
/// rule. Degrees appear only in reference tables and on HUDs.
struct OrbitalElements {
    /// a — half the long axis of the ellipse. Sets the size, and with GM sets
    /// the period. Negative for hyperbolic trajectories.
    double semiMajorAxis{0.0};

    /// e — shape. 0 is a circle, 0 < e < 1 an ellipse, 1 a parabola,
    /// > 1 a hyperbola (you are leaving).
    double eccentricity{0.0};

    /// i — tilt of the orbital plane against the reference plane. 0 is
    /// equatorial and prograde, pi/2 polar, pi equatorial and retrograde.
    double inclination{0.0};

    /// Omega (RAAN) — where the orbit crosses the reference plane going north.
    double longitudeOfAscendingNode{0.0};

    /// omega — rotation of the ellipse within its own plane; where periapsis
    /// sits relative to the ascending node.
    double argumentOfPeriapsis{0.0};

    /// M0 — mean anomaly at `epoch`. The only element that advances with time,
    /// and it advances perfectly linearly, which is exactly why it is the one
    /// we store rather than the true anomaly.
    double meanAnomalyAtEpoch{0.0};

    /// The instant at which meanAnomalyAtEpoch applies.
    Epoch epoch{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Derived quantities. Each needs GM because an orbit's *shape* is geometry but
// its *timing* depends on the mass it goes around.
// ─────────────────────────────────────────────────────────────────────────────

/// Distance at the closest point of the orbit, metres.
[[nodiscard]] double periapsisRadius(const OrbitalElements& e) noexcept;

/// Distance at the farthest point, metres. Infinite for e >= 1.
[[nodiscard]] double apoapsisRadius(const OrbitalElements& e) noexcept;

/// n — mean angular rate, rad/s. sqrt(GM / a^3).
[[nodiscard]] double meanMotion(const OrbitalElements& e, double gm) noexcept;

/// Orbital period in seconds. Infinite for e >= 1.
[[nodiscard]] double orbitalPeriod(const OrbitalElements& e, double gm) noexcept;

/// Mean anomaly advanced to time t. Wrapped to [0, 2pi).
[[nodiscard]] double meanAnomalyAt(const OrbitalElements& e, double gm, Epoch t) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Kepler's equation
// ─────────────────────────────────────────────────────────────────────────────

/// Solve M = E - e*sin(E) for the eccentric anomaly E, in radians.
///
/// This equation has no closed-form solution — Kepler said as much in 1609 and
/// nobody has improved on that since. It is solved numerically, here by
/// Newton-Raphson, which converges quadratically and typically needs three or
/// four iterations.
///
/// The starting guess matters. For low eccentricity, E ≈ M + e*sin(M) is
/// excellent. Near e = 1 the function flattens around periapsis and a naive
/// guess can send Newton off to a wrong root or oscillate forever, so high
/// eccentricities start from pi instead. If Newton fails to converge within
/// the iteration cap we fall back to bisection, which is slower but cannot
/// fail on a monotonic function — and this one is monotonic for all e < 1.
///
/// \param meanAnomaly   M, radians. Any value; wrapped internally.
/// \param eccentricity  e, in [0, 1).
[[nodiscard]] double solveKeplerEquation(double meanAnomaly, double eccentricity) noexcept;

/// Eccentric anomaly E to true anomaly nu, radians.
[[nodiscard]] double trueAnomalyFromEccentric(double eccentricAnomaly,
                                              double eccentricity) noexcept;

/// True anomaly nu to eccentric anomaly E, radians.
[[nodiscard]] double eccentricAnomalyFromTrue(double trueAnomaly,
                                              double eccentricity) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// The conversions
// ─────────────────────────────────────────────────────────────────────────────

/// Elements to state vector, evaluated at time \p t.
///
/// \param gm  the standard gravitational parameter of the *central* body —
///            strictly GM_central + GM_orbiting for a true two-body problem.
[[nodiscard]] StateVector stateFromElements(const OrbitalElements& elements,
                                            double gm,
                                            Epoch t) noexcept;

/// State vector to elements. The inverse of stateFromElements.
///
/// Degenerate orbits are handled by convention rather than by failing:
///   * circular (e ≈ 0): argumentOfPeriapsis is set to 0 and the angle is
///     folded into the mean anomaly, since periapsis is undefined.
///   * equatorial (i ≈ 0): longitudeOfAscendingNode is set to 0 for the same
///     reason — the ascending node does not exist.
/// Both conventions are standard, and both round-trip correctly through
/// stateFromElements, which is what the tests check.
[[nodiscard]] OrbitalElements elementsFromState(const StateVector& state,
                                                double gm,
                                                Epoch t) noexcept;

/// Specific orbital energy, J/kg. Conserved exactly on a Kepler orbit, so a
/// change in this value is the single most useful diagnostic for an integrator
/// that is quietly going wrong.
[[nodiscard]] double specificOrbitalEnergy(const StateVector& state, double gm) noexcept;

/// Specific angular momentum h = r x v, m^2/s. Also conserved, and it defines
/// the orbital plane.
[[nodiscard]] Vec3 specificAngularMomentum(const StateVector& state) noexcept;

/// Wrap an angle to [0, 2pi).
[[nodiscard]] double wrapToTwoPi(double radians) noexcept;

/// Wrap an angle to [-pi, pi).
[[nodiscard]] double wrapToPi(double radians) noexcept;

}  // namespace omma
