// ─────────────────────────────────────────────────────────────────────────────
// World — the simulation, one tick at a time.
//
// Owns the celestial bodies (looked up), the spacecraft (integrated), and the
// clock. Everything above this is a client: a renderer, a scenario runner, a
// Monte Carlo sweep.
//
// THE TICK IS THE CONTRACT
//
//     step()   advance exactly one fixed dt. Deterministic. No wall clock, no
//              randomness that is not seeded, no I/O.
//
// Everything the World does happens inside step(), in a fixed order, and that
// order is part of the contract because changing it changes the answers:
//
//     1. advance the clock
//     2. refresh the gravity environment (implicitly, per integrator stage)
//     3. integrate every spacecraft, applying thrust
//     4. burn propellant, expire finished burns
//     5. update each craft's central body and orbital elements
//     6. detect collisions
//
// Doing 6 before 3 would let a craft pass through a planet during the step it
// hit it. Doing 4 before 3 would burn propellant the burn had not yet used.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/sim_clock.hpp"
#include "physics/gravity_field.hpp"
#include "physics/integrator.hpp"
#include "physics/solar_system.hpp"
#include "sim/spacecraft.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace omma {

/// Something worth telling the world about. Recorded during a tick, drained by
/// whoever cares.
///
/// An event queue rather than a callback, for two reasons. Callbacks fire in the
/// middle of a tick, when the world is halfway between consistent states — and a
/// listener that mutates the world from inside step() is a class of bug that is
/// very hard to reason about. And a queue is recordable, which means a replay can
/// assert that the same things happened.
struct SimEvent {
    enum class Kind : std::uint8_t {
        Launched,
        BurnStarted,
        BurnEnded,
        PropellantExhausted,
        Collided,        ///< hit a celestial body
        Destroyed,
    };

    Kind         kind{Kind::Launched};
    SpacecraftId craft{SpacecraftId::invalid()};
    Epoch        when{};
    /// Body index for Collided; otherwise unused.
    std::size_t  bodyIndex{0};
    /// Free-text detail for logs. Never parsed.
    std::string  detail;
};

/// Where and how to put something in orbit.
///
/// Deliberately expressed as an ORBIT, not as a rocket launch. Modelling ascent
/// through an atmosphere is a different simulator: it needs drag, a thrust curve,
/// a steering program and a launch site rotating with the planet. What this
/// simulator is about starts at orbit insertion, so a launch here means "a
/// spacecraft appears on this orbit, having spent this much delta-v to get there".
///
/// The delta-v is charged honestly against the propellant budget, so a
/// high-energy orbit really does leave less in the tank.
struct LaunchRequest {
    std::string name{"SAT"};
    BodyId      aroundBody{BodyId::Earth};

    double altitudeMetres{400.0e3};       ///< above the body's mean radius
    double eccentricity{0.0};
    double inclinationRadians{0.0};
    double longitudeOfAscendingNodeRadians{0.0};
    double argumentOfPeriapsisRadians{0.0};
    double meanAnomalyRadians{0.0};

    double dryMassKg{200.0};
    double propellantKg{50.0};
    double maxThrustNewtons{22.0};
    double exhaustVelocity{2200.0};
};

class World {
public:
    /// \param fixedStep  the physics timestep. One second is a good default for
    ///                   low orbits; see the note on stability in the .cpp.
    World(SolarSystem system, Duration fixedStep, Epoch start = Epoch::j2000());

    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ── the tick ────────────────────────────────────────────────────────────

    /// Advance exactly one fixed step. The only way time moves.
    void step();

    /// Advance n steps. Identical to calling step() n times — asserted by a test,
    /// because a replay driven by recorded step counts depends on it.
    void step(std::int64_t n);

    // ── spacecraft ──────────────────────────────────────────────────────────

    /// Insert a spacecraft on the requested orbit. Returns an invalid id if the
    /// orbit is impossible (below the surface, or eccentricity >= 1).
    SpacecraftId launch(const LaunchRequest& request);

    /// Command a burn. Duration rather than delta-v: a thruster has a valve and a
    /// clock, and the delta-v is what comes out.
    bool commandBurn(SpacecraftId id, ThrustCommand::Frame frame,
                     double throttle, Duration duration);

    /// Command a burn sized to deliver a target delta-v, computed through the
    /// rocket equation. Fails if the craft cannot afford it.
    bool commandDeltaV(SpacecraftId id, ThrustCommand::Frame frame,
                       double deltaVMps);

    void cancelBurn(SpacecraftId id);

    [[nodiscard]] const Spacecraft* find(SpacecraftId id) const noexcept;
    [[nodiscard]] Spacecraft* find(SpacecraftId id) noexcept;

    [[nodiscard]] const std::vector<Spacecraft>& spacecraft() const noexcept {
        return spacecraft_;
    }

    /// Orbital elements of a craft, measured against whichever body currently
    /// dominates its gravity. Recomputed on demand rather than cached, because a
    /// cached copy is a second source of truth waiting to disagree.
    [[nodiscard]] OrbitalElements elementsOf(const Spacecraft& craft) const noexcept;

    /// State relative to the craft's central body, which is what "prograde" and
    /// "altitude" are actually about.
    [[nodiscard]] StateVector relativeState(const Spacecraft& craft) const noexcept;

    /// Height above the central body's mean radius, metres. Negative means below
    /// the surface, which means you have already crashed.
    [[nodiscard]] double altitudeOf(const Spacecraft& craft) const noexcept;

    // ── access ──────────────────────────────────────────────────────────────

    [[nodiscard]] const SolarSystem& system() const noexcept { return system_; }
    [[nodiscard]] const SimClock& clock() const noexcept { return clock_; }
    [[nodiscard]] Epoch now() const noexcept { return clock_.now(); }
    [[nodiscard]] Integrator integrator() const noexcept { return integrator_; }
    void setIntegrator(Integrator method) noexcept { integrator_ = method; }

    /// Take everything that happened since the last drain.
    [[nodiscard]] std::vector<SimEvent> drainEvents();
    [[nodiscard]] const std::vector<SimEvent>& peekEvents() const noexcept {
        return events_;
    }

private:
    void integrateSpacecraft(Spacecraft& craft, double dt);
    void updateCentralBody(Spacecraft& craft);
    void detectCollisions();
    void emit(SimEvent event) { events_.push_back(std::move(event)); }

    SolarSystem            system_;
    GravityField           gravity_;
    SimClock               clock_;
    Integrator             integrator_{Integrator::RungeKutta4};
    std::vector<Spacecraft> spacecraft_;
    std::vector<SimEvent>  events_;
    std::uint32_t          nextGeneration_{1};
};

}  // namespace omma
