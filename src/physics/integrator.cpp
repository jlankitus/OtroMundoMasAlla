#include "physics/integrator.hpp"

#include <cmath>

namespace omma {
namespace {

/// Gravity plus whatever else, at a given time and place.
[[nodiscard]] inline Vec3 accelerationAt(GravityField& field, Epoch t,
                                        const Vec3& position,
                                        const Vec3& extra) noexcept {
    field.refresh(t);
    return field.accelerationAt(position) + extra;
}

/// Convert a double-seconds offset into an exact Duration for the sub-step
/// epochs an integrator needs.
///
/// Worth noticing: the half-step epoch is computed from the START epoch plus an
/// offset, never by accumulating half-steps. Time is still exact integer
/// nanoseconds even inside a single RK4 step.
[[nodiscard]] inline Epoch offsetBy(Epoch t, double seconds) noexcept {
    return t + fromSeconds(seconds);
}

void explicitEuler(StateVector& s, GravityField& field, Epoch t, double dt,
                   const Vec3& extra) noexcept {
    const Vec3 a = accelerationAt(field, t, s.position, extra);
    // Position updated from the OLD velocity, then velocity from the old
    // acceleration. On a curving path this consistently overshoots outward, so
    // the orbit gains energy on every single step.
    s.position += s.velocity * dt;
    s.velocity += a * dt;
}

void semiImplicitEuler(StateVector& s, GravityField& field, Epoch t, double dt,
                       const Vec3& extra) noexcept {
    const Vec3 a = accelerationAt(field, t, s.position, extra);
    // Velocity FIRST, then position from the new velocity. That single
    // reordering makes the method symplectic, and turns an orbit that spirals
    // apart into one that stays closed indefinitely. Same cost, same line
    // count, completely different long-term behaviour.
    s.velocity += a * dt;
    s.position += s.velocity * dt;
}

void velocityVerlet(StateVector& s, GravityField& field, Epoch t, double dt,
                    const Vec3& extra) noexcept {
    const Vec3 a0 = accelerationAt(field, t, s.position, extra);
    s.position += s.velocity * dt + a0 * (0.5 * dt * dt);
    const Vec3 a1 = accelerationAt(field, offsetBy(t, dt), s.position, extra);
    s.velocity += (a0 + a1) * (0.5 * dt);
}

void rungeKutta4(StateVector& s, GravityField& field, Epoch t, double dt,
                 const Vec3& extra) noexcept {
    const double half = 0.5 * dt;
    const Epoch midpoint = offsetBy(t, half);
    const Epoch endpoint = offsetBy(t, dt);

    // The four stages. Note each one asks the environment where the planets are
    // at a DIFFERENT instant, including two evaluations at the midpoint. An
    // ephemeris answers that exactly and for free; a fully integrated solar
    // system would have to march every planet through the same four stages in
    // lockstep. This is the concrete payoff of the IEphemeris split.
    const Vec3 k1v = accelerationAt(field, t, s.position, extra);
    const Vec3 k1r = s.velocity;

    const Vec3 k2v = accelerationAt(field, midpoint, s.position + k1r * half, extra);
    const Vec3 k2r = s.velocity + k1v * half;

    const Vec3 k3v = accelerationAt(field, midpoint, s.position + k2r * half, extra);
    const Vec3 k3r = s.velocity + k2v * half;

    const Vec3 k4v = accelerationAt(field, endpoint, s.position + k3r * dt, extra);
    const Vec3 k4r = s.velocity + k3v * dt;

    const double sixth = dt / 6.0;
    s.position += (k1r + (k2r + k3r) * 2.0 + k4r) * sixth;
    s.velocity += (k1v + (k2v + k3v) * 2.0 + k4v) * sixth;
}

}  // namespace

std::string_view integratorName(Integrator method) noexcept {
    switch (method) {
        case Integrator::ExplicitEuler:     return "explicit Euler";
        case Integrator::SemiImplicitEuler: return "semi-implicit Euler";
        case Integrator::VelocityVerlet:    return "velocity Verlet";
        case Integrator::RungeKutta4:       return "RK4";
    }
    return "unknown";
}

int accelerationSamplesPerStep(Integrator method) noexcept {
    switch (method) {
        case Integrator::ExplicitEuler:     return 1;
        case Integrator::SemiImplicitEuler: return 1;
        case Integrator::VelocityVerlet:    return 2;
        case Integrator::RungeKutta4:       return 4;
    }
    return 1;
}

void integrate(Integrator method, StateVector& state, GravityField& field,
               Epoch t, double dt, Vec3 extraAcceleration) noexcept {
    switch (method) {
        case Integrator::ExplicitEuler:
            explicitEuler(state, field, t, dt, extraAcceleration);
            return;
        case Integrator::SemiImplicitEuler:
            semiImplicitEuler(state, field, t, dt, extraAcceleration);
            return;
        case Integrator::VelocityVerlet:
            velocityVerlet(state, field, t, dt, extraAcceleration);
            return;
        case Integrator::RungeKutta4:
            rungeKutta4(state, field, t, dt, extraAcceleration);
            return;
    }
}

double specificEnergy(const StateVector& state, const GravityField& field) noexcept {
    double potential = 0.0;
    for (const GravitySource& source : field.sources()) {
        const double r = distance(source.position, state.position);
        if (r > 0.0) {
            potential -= source.gm / r;
        }
    }
    return 0.5 * state.velocity.normSquared() + potential;
}

}  // namespace omma
