// ─────────────────────────────────────────────────────────────────────────────
// World — the simulation, one tick at a time. Owns the celestial bodies
// (looked up), the spacecraft (integrated), and the clock; everything above is
// a client. See docs/DESIGN.md §6.
//
// step() advances exactly one fixed dt, deterministically, in a fixed order
// that is part of the contract because changing it changes the answers:
//     1. advance the clock
//     2. refresh the gravity environment (implicitly, per integrator stage)
//     3. integrate every spacecraft, applying thrust
//     4. burn propellant, expire finished burns
//     5. update each craft's central body and orbital elements
//     6. detect collisions
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/sim_clock.hpp"
#include "physics/gravity_field.hpp"
#include "physics/ground_track.hpp"
#include "physics/integrator.hpp"
#include "physics/solar_system.hpp"
#include "sim/spacecraft.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace omma {

/// Something worth telling the world about. Recorded during a tick, drained by
/// whoever cares. A queue rather than callbacks: callbacks fire mid-tick between
/// consistent states, and a queue is recordable, so a replay can assert on it.
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

/// Where and how to put something in orbit. Models orbit INSERTION, not ascent —
/// atmospheric ascent is a different simulator. The delta-v spent reaching the
/// orbit is charged against the propellant budget.
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
    double dragCoefficient{2.2};
    double crossSectionM2{1.5};
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

    /// Put a spacecraft ON THE PAD: at rest on the rotating surface of \p body
    /// with the ascent autopilot armed. Unlike launch(), nothing is inserted
    /// anywhere — the vehicle has to fly there, through the same gravity, drag
    /// and thrust every other craft experiences.
    SpacecraftId launchFromSurface(BodyId body, const SurfaceLaunchRequest& request,
                                   std::string name = "ASCENT");

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

    /// Subsatellite point on the craft's central body. Longitude is meaningful
    /// only for bodies with a rotation model (Earth, Mars); for the rest it is
    /// the inertial longitude, which still bounds latitude correctly.
    [[nodiscard]] LatLon groundTrackOf(const Spacecraft& craft) const noexcept;

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

/// The step cap when nothing is integrated: effectively infinite, because
/// advancing an all-analytic world is one integer addition regardless of n.
inline constexpr std::int64_t kUnboundedStepBudget = 1'000'000'000'000LL;

/// Steps per frame an interactive client can afford, in CRAFT-steps divided
/// by fleet size: a fixed cap holds the frame rate for one craft and
/// collapses it for twelve. Shared by every real-time client so they all
/// fall behind honestly, and identically.
[[nodiscard]] std::int64_t interactiveStepBudget(const World& world) noexcept;

}  // namespace omma
