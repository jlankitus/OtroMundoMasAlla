// ─────────────────────────────────────────────────────────────────────────────
// Numerical integrators.
//
// Four of them, three of which are useful and one of which is a warning.
//
// RK4 versus symplectic is not a quality ordering, it is a trade:
//
//               error per step        error over a million orbits
//   Euler       O(dt)                unbounded, orbit spirals outward
//   Verlet      O(dt^2)              BOUNDED — energy oscillates, never drifts
//   RK4         O(dt^4)              small but SECULAR — slowly accumulates
//
// RK4 wins over hours. Velocity Verlet wins over eons, despite being far less
// accurate per step, because it is symplectic: it exactly conserves a slightly
// wrong energy rather than approximately conserving the right one. The orbit
// stays closed forever. Long-term solar-system integrations use the symplectic
// family for exactly this reason, and short-arc spacecraft work uses RK4.
//
// Explicit Euler is here as a cautionary baseline. It consistently undershoots
// a curving path, so it gains energy and every orbit spirals out. There is a
// test asserting that it does, and the headless demo prints the drift side by
// side. It is much more convincing to see the naive method fail than to be told
// it would.
//
// THRUST IS HELD CONSTANT ACROSS A STEP
// A zero-order hold. Standard practice, and with dt = 1 s against burns that
// last minutes it is well below the integration error. Passing a callback
// instead would allow thrust to vary within a step and would also put a
// std::function call in the innermost loop, four times per spacecraft per step.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/gravity_field.hpp"
#include "physics/state_vector.hpp"

#include <string_view>

namespace omma {

enum class Integrator {
    /// Explicit (forward) Euler. One acceleration sample. Wrong on purpose:
    /// kept as a baseline and as a test that our error metrics can detect a
    /// bad integrator at all.
    ExplicitEuler,
    /// Semi-implicit (symplectic) Euler. Same cost as explicit Euler, vastly
    /// better behaved, because updating velocity BEFORE position makes it
    /// symplectic. One line of difference, an entirely different character.
    SemiImplicitEuler,
    /// Velocity Verlet. Two samples, symplectic, bounded energy error.
    VelocityVerlet,
    /// Classical Runge-Kutta 4th order. Four samples. The workhorse.
    RungeKutta4,
};

[[nodiscard]] std::string_view integratorName(Integrator method) noexcept;

/// Acceleration samples consumed per step. Multiply by the step count to
/// compare integrators at equal cost rather than at equal step size — which is
/// the only fair comparison, and one that flatters RK4 much less.
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

/// Total specific energy of a state in a gravity field, J/kg.
///
/// Kinetic plus the summed potential of every source. THE diagnostic: a closed
/// orbit has constant total energy, so any change is integration error made
/// visible. Requires a refreshed field.
[[nodiscard]] double specificEnergy(const StateVector& state,
                                   const GravityField& field) noexcept;

}  // namespace omma
