// ─────────────────────────────────────────────────────────────────────────────
// Numerical integrators. RK4 versus symplectic is a trade, not a ranking:
// RK4's O(dt^4) error is small but SECULAR, Verlet's O(dt^2) error is larger
// but BOUNDED (symplectic — energy oscillates, never drifts), so RK4 wins
// over hours and Verlet over eons. Explicit Euler is a cautionary baseline:
// it gains energy on every step, and a test asserts that it does. Thrust is
// held constant across a step (zero-order hold) — well below integration
// error at dt = 1 s, and it keeps std::function out of the innermost loop.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/gravity_field.hpp"
#include "physics/state_vector.hpp"

#include <string_view>

namespace omma {

enum class Integrator {
    /// Explicit (forward) Euler. One sample. Wrong on purpose: a baseline
    /// proving the error metrics can detect a bad integrator at all.
    ExplicitEuler,
    /// Semi-implicit (symplectic) Euler. Same cost as explicit Euler, vastly
    /// better behaved: updating velocity BEFORE position makes it symplectic.
    SemiImplicitEuler,
    /// Velocity Verlet. Two samples, symplectic, bounded energy error.
    VelocityVerlet,
    /// Classical Runge-Kutta 4th order. Four samples. The workhorse.
    RungeKutta4,
};

[[nodiscard]] std::string_view integratorName(Integrator method) noexcept;

/// Acceleration samples consumed per step, for comparing integrators at
/// equal cost rather than equal step size — the only fair comparison.
[[nodiscard]] int accelerationSamplesPerStep(Integrator method) noexcept;

/// Advance \p state by \p dt seconds.
///
/// \param field  refreshed internally at each stage the method needs. Callers
///               do not manage its two-phase lifecycle.
/// \param t      the epoch at the START of the step.
/// \param extraAcceleration  thrust, drag, anything beyond point-mass gravity,
///               in m/s^2, held constant across the step.
void integrate(Integrator method,
               StateVector& state,
               GravityField& field,
               Epoch t,
               double dt,
               Vec3 extraAcceleration = Vec3::zero()) noexcept;

/// Total specific energy of a state in a gravity field, J/kg: kinetic plus
/// the summed potential of every source. Constant on a closed orbit, so any
/// change is integration error made visible. Requires a refreshed field.
[[nodiscard]] double specificEnergy(const StateVector& state,
                                   const GravityField& field) noexcept;

}  // namespace omma
