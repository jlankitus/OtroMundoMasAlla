// ─────────────────────────────────────────────────────────────────────────────
// Spacecraft — a thing we integrate, as opposed to a thing we look up.
//
// WHY THIS IS A STRUCT OF PLAIN DATA AND NOT A CLASS HIERARCHY
// The obvious design is a Satellite base class with virtual update(), and
// subclasses for comms relays, imagers, interceptors. It does not survive the
// requirements: this needs to reach thousands of craft, and a vector of
// unique_ptr<Satellite> with a virtual call per craft per RK4 stage is the exact
// shape that makes "OOP is slow" true.
//
// Behaviour differences will arrive as COMPONENTS and as a state machine on
// `mode`, not as subclasses. That keeps the state itself a flat, trivially
// copyable block that can be moved into a structure-of-arrays layout later
// without touching any of the logic that reads it.
//
// PROPULSION IS MODELLED, NOT FAKED
// Thrust burns propellant, propellant is mass, and mass divides into force. A
// spacecraft therefore gets *lighter* and accelerates *harder* as a burn
// proceeds. Ignoring that is the difference between "delta-v" being a real budget
// and being a cheat code, and the budget is what makes every later decision
// interesting.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/state_vector.hpp"

#include <cstdint>
#include <string>

namespace omma {

/// Stable handle for a spacecraft.
///
/// An index would dangle the moment the container reorders — and it will reorder,
/// because a spatial sort is how conjunction detection will stay affordable at
/// ten thousand objects. A generation counter makes a stale handle detectable
/// rather than silently pointing at whoever moved into the slot.
struct SpacecraftId {
    std::uint32_t index{0};
    std::uint32_t generation{0};

    friend constexpr bool operator==(SpacecraftId, SpacecraftId) noexcept = default;

    [[nodiscard]] static constexpr SpacecraftId invalid() noexcept {
        return SpacecraftId{0xFFFF'FFFFu, 0};
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return index != 0xFFFF'FFFFu;
    }
};

/// How a spacecraft's state is being advanced.
///
/// The State pattern, and the reason unbounded time warp is possible at all: a
/// coasting craft can be frozen onto its own Kepler ellipse, at which point its
/// position is a function call and warp costs nothing. Under thrust it must be
/// integrated, and warp is limited to what the integrator can chew through.
enum class PropagationMode : std::uint8_t {
    /// Numerically integrated against the full gravity field.
    Integrated,
    /// Frozen onto an analytic two-body ellipse. Not implemented yet; the mode
    /// exists so the transition has somewhere to go.
    OnRails,
    /// Hit something, or ran into a planet. Kept for the wreckage.
    Destroyed,
};

/// What a spacecraft is currently trying to do with its engine.
struct ThrustCommand {
    /// Direction, in the body's local orbital frame rather than the world frame,
    /// because "burn prograde" is what a mission plan actually says. Resolved
    /// against the current state each step.
    enum class Frame : std::uint8_t {
        /// Along the velocity vector. Raises the far side of the orbit.
        Prograde,
        /// Against it. Lowers the far side.
        Retrograde,
        /// Along the orbital angular momentum. Changes inclination.
        Normal,
        /// Against it.
        AntiNormal,
        /// Away from the body being orbited.
        RadialOut,
        /// Toward it.
        RadialIn,
        /// A literal world-frame direction, for anything the above cannot say.
        World,
    };

    Frame  frame{Frame::Prograde};
    /// Fraction of maximum thrust, clamped to [0, 1].
    double throttle{0.0};
    /// Only used when frame == World. Need not be normalised.
    Vec3   worldDirection{};

    /// Simulated instant at which the burn should stop. A burn is specified as a
    /// duration rather than a delta-v because that is what a thruster actually
    /// has: a valve and a clock. The delta-v is the *outcome*.
    Epoch  cutoff{};
    bool   active{false};
};

struct Spacecraft {
    std::string  name;
    SpacecraftId id{SpacecraftId::invalid()};

    /// Position and velocity in the root frame. The authoritative state.
    StateVector  state{};

    PropagationMode mode{PropagationMode::Integrated};
    ThrustCommand   thrust{};

    // ── mass and propulsion ─────────────────────────────────────────────────
    /// Everything that is not propellant, kg.
    double dryMassKg{200.0};
    /// Remaining propellant, kg. Never negative.
    double propellantKg{50.0};
    /// Maximum thrust, newtons.
    double maxThrustNewtons{22.0};
    /// Effective exhaust velocity, m/s. Ties thrust to propellant flow through
    /// the rocket equation: mdot = F / ve. A monopropellant hydrazine thruster is
    /// around 2200 m/s; an ion engine ten times that and a thousandth the thrust.
    double exhaustVelocity{2200.0};

    /// Which body this craft's orbital elements are measured against. Set from
    /// the dominant gravity source each step, so it follows the craft across a
    /// sphere-of-influence boundary.
    std::size_t centralBodyIndex{0};

    // ── bookkeeping ─────────────────────────────────────────────────────────
    /// Total delta-v spent, m/s. The real currency; worth tracking explicitly
    /// rather than inferring from mass, because staging will break that link.
    double deltaVSpentMps{0.0};
    Epoch  launchEpoch{};

    [[nodiscard]] double totalMassKg() const noexcept { return dryMassKg + propellantKg; }

    /// Acceleration the engine can currently produce, m/s^2.
    [[nodiscard]] double maxAccelerationMps2() const noexcept {
        return maxThrustNewtons / totalMassKg();
    }

    /// Remaining delta-v from the rocket equation, dv = ve * ln(m0 / m1).
    ///
    /// Tsiolkovsky. The logarithm is why staging exists: to double your delta-v
    /// you do not double your propellant, you square the mass ratio.
    [[nodiscard]] double remainingDeltaVMps() const noexcept;

    [[nodiscard]] bool isAlive() const noexcept {
        return mode != PropagationMode::Destroyed;
    }
    [[nodiscard]] bool hasPropellant() const noexcept { return propellantKg > 0.0; }
};

/// Resolve a thrust command into a world-frame unit vector for the given state.
///
/// \param state    the craft's state RELATIVE to its central body. Prograde is
///                 meaningless in absolute heliocentric terms when what you mean
///                 is "along my orbit around Earth".
[[nodiscard]] Vec3 thrustDirection(const ThrustCommand& command,
                                  const StateVector& state) noexcept;

}  // namespace omma
