// ─────────────────────────────────────────────────────────────────────────────
// Spacecraft — a thing we integrate, as opposed to a thing we look up.
//
// A flat POD struct, not a class hierarchy: behaviour arrives as components and
// a state machine on `mode`, keeping the state trivially copyable and ready for
// a structure-of-arrays layout. Propulsion is modelled, not faked: thrust burns
// propellant, propellant is mass, so a craft gets lighter and accelerates
// harder as a burn proceeds. See docs/DESIGN.md §5–6.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/state_vector.hpp"
#include "sim/ascent.hpp"

#include <cstdint>
#include <string>

namespace omma {

/// Stable handle for a spacecraft. A bare index would dangle when the container
/// reorders; the generation counter makes a stale handle detectable rather than
/// silently pointing at whoever moved into the slot.
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

/// How a spacecraft's state is being advanced. A coasting craft can be frozen
/// onto its own Kepler ellipse (OnRails), making warp free; under thrust it
/// must be integrated.
enum class PropagationMode : std::uint8_t {
    /// Numerically integrated against the full gravity field.
    Integrated,
    /// Frozen onto an analytic two-body ellipse. Not implemented yet.
    OnRails,
    /// Hit something, or ran into a planet. Kept for the wreckage.
    Destroyed,
};

/// What a spacecraft is currently trying to do with its engine.
struct ThrustCommand {
    /// Direction, in the craft's local orbital frame — "burn prograde" is what a
    /// mission plan says. Resolved against the current state each step.
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

    /// Simulated instant at which the burn should stop.
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
    /// The ascent autopilot, when this craft was launched from a surface.
    /// Guidance writes to `thrust`; it never bypasses the physics.
    AscentProgram   ascent{};

    // ── mass and propulsion ─────────────────────────────────────────────────
    /// Everything that is not propellant, kg.
    double dryMassKg{200.0};
    /// Remaining propellant, kg. Never negative.
    double propellantKg{50.0};
    /// Maximum thrust, newtons.
    double maxThrustNewtons{22.0};
    /// Effective exhaust velocity, m/s; ties thrust to propellant flow through
    /// mdot = F / ve. Hydrazine ~2200; an ion engine around ten times that.
    double exhaustVelocity{2200.0};

    // ── drag ────────────────────────────────────────────────────────────────
    /// Drag coefficient. ~2.2 for most satellites, and no one knows theirs to
    /// better than ~10%, so this default is as good as a measurement.
    double dragCoefficient{2.2};
    /// Cross-section presented to the flow, m^2. With the default masses this
    /// gives a ballistic coefficient m/(Cd A) ~ 85 kg/m^2 — a typical smallsat.
    double crossSectionM2{1.5};

    /// Body the craft's elements are measured against; set from the dominant
    /// gravity source each step, so it follows sphere-of-influence crossings.
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
    [[nodiscard]] double remainingDeltaVMps() const noexcept;

    [[nodiscard]] bool isAlive() const noexcept {
        return mode != PropagationMode::Destroyed;
    }
    [[nodiscard]] bool hasPropellant() const noexcept { return propellantKg > 0.0; }
};

/// Resolve a thrust command into a world-frame unit vector for the given state.
/// \param state  the craft's state RELATIVE to its central body — prograde means
///               "along my orbit around Earth", not the heliocentric velocity.
[[nodiscard]] Vec3 thrustDirection(const ThrustCommand& command,
                                  const StateVector& state) noexcept;

}  // namespace omma
