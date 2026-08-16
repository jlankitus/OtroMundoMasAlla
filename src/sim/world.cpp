#include "sim/world.hpp"

#include "core/units.hpp"
#include "physics/atmosphere.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace omma {
namespace {

/// Below this throttle a burn is treated as off, so a rounding-error throttle
/// cannot dribble propellant away over a million steps.
constexpr double kMinimumThrottle = 1e-6;

}  // namespace

World::World(SolarSystem system, Duration fixedStep, Epoch start)
    : system_{std::move(system)},
      gravity_{makeGravityField(system_)},
      clock_{fixedStep, start} {
    // Step size: 1 s is comfortable for LEO, not for a low periapsis on a highly
    // eccentric orbit. The integrator does not adapt (determinism), so a scenario
    // must choose a step appropriate to its tightest orbit.
    gravity_.refresh(start);
}

void World::step() {
    // 1. Time advances first, so everything below agrees on what "now" is.
    const Epoch before = clock_.now();
    clock_.step();
    const double dt = clock_.fixedStepSeconds();

    // 2/3. Integrate every craft. The gravity field is refreshed inside the
    // integrator, once per stage, at the sub-step epochs RK4 asks for.
    for (Spacecraft& craft : spacecraft_) {
        if (craft.mode != PropagationMode::Integrated) {
            continue;
        }
        integrateSpacecraft(craft, dt);
    }

    // 4. Expire finished burns. Done after integrating, because the burn was
    // active for the step we just took.
    for (Spacecraft& craft : spacecraft_) {
        if (!craft.thrust.active) {
            continue;
        }
        if (clock_.now() >= craft.thrust.cutoff) {
            craft.thrust.active = false;
            craft.thrust.throttle = 0.0;
            emit({SimEvent::Kind::BurnEnded, craft.id, clock_.now(), 0, craft.name});
        } else if (!craft.hasPropellant()) {
            craft.thrust.active = false;
            craft.thrust.throttle = 0.0;
            emit({SimEvent::Kind::PropellantExhausted, craft.id, clock_.now(), 0,
                  craft.name});
        }
    }

    // 5. Which body each craft belongs to now.
    for (Spacecraft& craft : spacecraft_) {
        if (craft.isAlive()) {
            updateCentralBody(craft);
        }
    }

    // 6. Collisions last, against the state the craft actually ended up in.
    detectCollisions();

    static_cast<void>(before);
}

void World::step(std::int64_t n) {
    for (std::int64_t i = 0; i < n; ++i) {
        step();
    }
}

void World::integrateSpacecraft(Spacecraft& craft, double dt) {
    Vec3 extraAcceleration{};
    const Epoch stepStart = clock_.now() - clock_.fixedStep();

    if (craft.thrust.active && craft.thrust.throttle > kMinimumThrottle
        && craft.hasPropellant()) {

        // Duty cycle: honour the burn cutoff within the step. A cutoff almost
        // never lands on a step boundary, and a 0.19 s burn inside a 1 s step
        // must deliver 19% of the step's impulse, not 100%. Zero-order hold:
        // the impulse is right, its distribution within the step is smeared.
        const double remaining = toSeconds(craft.thrust.cutoff - stepStart);
        const double activeSeconds = std::clamp(remaining, 0.0, dt);
        const double dutyCycle = activeSeconds / dt;
        // Direction is resolved in the craft's LOCAL orbital frame: "prograde"
        // means along its orbit around the central body, not its heliocentric
        // velocity.
        const StateVector local = relativeState(craft);
        const Vec3 direction = thrustDirection(craft.thrust, local);

        const double throttle = std::clamp(craft.thrust.throttle, 0.0, 1.0);
        const double force = craft.maxThrustNewtons * throttle;

        // Propellant flow from the thrust and the exhaust velocity: mdot = F/ve,
        // charged for the time the engine was actually on.
        const double burned = std::min(craft.propellantKg,
                                       (force / craft.exhaustVelocity) * activeSeconds);

        // Midpoint-rule mass. Holding the start-of-step mass throughout delivers
        // dv = dm/m0 instead of ve*ln(m0/m1), a systematic under-delivery;
        // evaluating mass halfway through this step's propellant fixes it.
        const double massMidpoint = craft.totalMassKg() - 0.5 * burned;
        const double acceleration = force / massMidpoint;

        extraAcceleration = direction * (acceleration * dutyCycle);

        craft.propellantKg -= burned;
        craft.deltaVSpentMps += acceleration * activeSeconds;
    }

    // Drag, where there is an atmosphere to fly through. Zero-order hold like
    // thrust: evaluated at the step start and held constant across it, which
    // at LEO speeds and a 1 s step is a ~0.1% smear on a force that is itself
    // order-of-magnitude by nature.
    const auto& central = *system_.bodies()[craft.centralBodyIndex];
    if (central.hasAtmosphere()) {
        const StateVector local = relativeState(craft);
        const double density =
            earthAtmosphericDensity(local.position.norm() - central.meanRadius());
        if (density > 0.0) {
            // The atmosphere co-rotates with the body, so drag opposes the
            // AIR-relative velocity: v_rel = v - omega x r, omega along +z.
            Vec3 airRelative = local.velocity;
            if (const double period = central.siderealRotationPeriod(); period > 0.0) {
                const double omega = constants::kTwoPi / period;
                airRelative.x += omega * local.position.y;
                airRelative.y -= omega * local.position.x;
            }
            const double speed = airRelative.norm();
            const double ballistic = craft.dragCoefficient * craft.crossSectionM2
                                   / craft.totalMassKg();
            extraAcceleration =
                extraAcceleration - airRelative * (0.5 * density * speed * ballistic);
        }
    }

    integrate(integrator_, craft.state, gravity_, stepStart, dt, extraAcceleration);
}

void World::updateCentralBody(Spacecraft& craft) {
    gravity_.refresh(clock_.now());
    const std::size_t dominant = gravity_.dominantSourceIndex(craft.state.position);
    if (dominant != GravityField::npos) {
        craft.centralBodyIndex = dominant;
    }
}

StateVector World::relativeState(const Spacecraft& craft) const noexcept {
    const auto& body = *system_.bodies()[craft.centralBodyIndex];
    return craft.state.relativeTo(body.sample(clock_.now()));
}

OrbitalElements World::elementsOf(const Spacecraft& craft) const noexcept {
    const auto& body = *system_.bodies()[craft.centralBodyIndex];
    // GM of the pair; the craft's own mass is negligible against any body here.
    const double gm = body.gravitationalParameter();
    return elementsFromState(relativeState(craft), gm, clock_.now());
}

double World::altitudeOf(const Spacecraft& craft) const noexcept {
    const auto& body = *system_.bodies()[craft.centralBodyIndex];
    return relativeState(craft).position.norm() - body.meanRadius();
}

LatLon World::groundTrackOf(const Spacecraft& craft) const noexcept {
    const auto& body = *system_.bodies()[craft.centralBodyIndex];
    return subsatellitePoint(
        relativeState(craft).position,
        rotationAngleAt(body.siderealRotationPeriod(), clock_.now()));
}

void World::detectCollisions() {
    // Celestial bodies only, for now; craft-versus-craft needs a spatial index.
    // A position test at end of step, not swept: a fast craft could cross a thin
    // shell within a step, but not a planet, which is what this is for.
    gravity_.refresh(clock_.now());

    for (Spacecraft& craft : spacecraft_) {
        if (!craft.isAlive()) {
            continue;
        }
        for (std::size_t i = 0; i < system_.size(); ++i) {
            const auto& body = *system_.bodies()[i];
            // The field was just refreshed at this instant, so read the cached
            // position rather than re-sampling (a Kepler solve per body per craft).
            const double distanceToCentre =
                distance(craft.state.position, gravity_.positionOf(i));
            if (distanceToCentre < body.meanRadius()) {
                craft.mode = PropagationMode::Destroyed;
                emit({SimEvent::Kind::Collided, craft.id, clock_.now(), i,
                      craft.name + " hit " + std::string{body.name()}});
                break;
            }
        }
    }
}

SpacecraftId World::launch(const LaunchRequest& request) {
    const auto bodyIndex = static_cast<std::size_t>(request.aroundBody);
    if (bodyIndex >= system_.size()) {
        return SpacecraftId::invalid();
    }
    const auto& body = *system_.bodies()[bodyIndex];

    const double periapsisRadius = body.meanRadius() + request.altitudeMetres;
    if (!(periapsisRadius > body.meanRadius()) || request.eccentricity < 0.0
        || request.eccentricity >= 1.0) {
        return SpacecraftId::invalid();
    }

    // Altitude is specified at PERIAPSIS: "400 km orbit" with eccentricity
    // means 400 km at the low point.
    OrbitalElements elements{};
    elements.semiMajorAxis = periapsisRadius / (1.0 - request.eccentricity);
    elements.eccentricity = request.eccentricity;
    elements.inclination = request.inclinationRadians;
    elements.longitudeOfAscendingNode = request.longitudeOfAscendingNodeRadians;
    elements.argumentOfPeriapsis = request.argumentOfPeriapsisRadians;
    elements.meanAnomalyAtEpoch = request.meanAnomalyRadians;
    elements.epoch = clock_.now();

    const double gm = body.gravitationalParameter();
    const StateVector relative = stateFromElements(elements, gm, clock_.now());
    const StateVector absolute = body.sample(clock_.now()) + relative;

    Spacecraft craft{};
    craft.name = request.name;
    craft.state = absolute;
    craft.dryMassKg = request.dryMassKg;
    craft.propellantKg = request.propellantKg;
    craft.maxThrustNewtons = request.maxThrustNewtons;
    craft.exhaustVelocity = request.exhaustVelocity;
    craft.dragCoefficient = request.dragCoefficient;
    craft.crossSectionM2 = request.crossSectionM2;
    craft.centralBodyIndex = bodyIndex;
    craft.launchEpoch = clock_.now();
    craft.id = SpacecraftId{static_cast<std::uint32_t>(spacecraft_.size()),
                            nextGeneration_++};

    spacecraft_.push_back(std::move(craft));
    emit({SimEvent::Kind::Launched, spacecraft_.back().id, clock_.now(), bodyIndex,
          spacecraft_.back().name});
    return spacecraft_.back().id;
}

const Spacecraft* World::find(SpacecraftId id) const noexcept {
    if (!id.isValid() || id.index >= spacecraft_.size()) {
        return nullptr;
    }
    const Spacecraft& craft = spacecraft_[id.index];
    return craft.id == id ? &craft : nullptr;
}

Spacecraft* World::find(SpacecraftId id) noexcept {
    return const_cast<Spacecraft*>(static_cast<const World*>(this)->find(id));
}

bool World::commandBurn(SpacecraftId id, ThrustCommand::Frame frame,
                        double throttle, Duration duration) {
    Spacecraft* craft = find(id);
    if (craft == nullptr || !craft->isAlive() || !craft->hasPropellant()) {
        return false;
    }
    if (duration <= Duration::zero()) {
        return false;
    }

    craft->thrust.frame = frame;
    craft->thrust.throttle = std::clamp(throttle, 0.0, 1.0);
    craft->thrust.cutoff = clock_.now() + duration;
    craft->thrust.active = true;

    emit({SimEvent::Kind::BurnStarted, id, clock_.now(), 0, craft->name});
    return true;
}

bool World::commandDeltaV(SpacecraftId id, ThrustCommand::Frame frame,
                          double deltaVMps) {
    Spacecraft* craft = find(id);
    if (craft == nullptr || !craft->isAlive() || !(deltaVMps > 0.0)) {
        return false;
    }
    if (deltaVMps > craft->remainingDeltaVMps()) {
        return false;   // cannot afford it; better to refuse than to half-burn
    }

    // Invert the rocket equation for the propellant needed, then the burn time
    // from the flow rate:
    //     dv = ve * ln(m0/m1)   =>   m1 = m0 * exp(-dv/ve)
    //     t  = (m0 - m1) / mdot,     mdot = F / ve
    const double m0 = craft->totalMassKg();
    const double m1 = m0 * std::exp(-deltaVMps / craft->exhaustVelocity);
    const double propellantNeeded = m0 - m1;
    const double flowRate = craft->maxThrustNewtons / craft->exhaustVelocity;
    if (!(flowRate > 0.0)) {
        return false;
    }
    const double seconds = propellantNeeded / flowRate;

    return commandBurn(id, frame, 1.0, fromSeconds(seconds));
}

void World::cancelBurn(SpacecraftId id) {
    if (Spacecraft* craft = find(id); craft != nullptr && craft->thrust.active) {
        craft->thrust.active = false;
        craft->thrust.throttle = 0.0;
        emit({SimEvent::Kind::BurnEnded, id, clock_.now(), 0, craft->name});
    }
}

std::vector<SimEvent> World::drainEvents() {
    std::vector<SimEvent> drained;
    drained.swap(events_);
    return drained;
}

std::int64_t interactiveStepBudget(const World& world) noexcept {
    // 6000 craft-steps/frame keeps a frame in the low milliseconds; measured
    // in the terminal client and integrator-bound, not renderer-bound.
    constexpr std::int64_t kCraftStepsPerFrame = 6'000;
    const auto fleet = static_cast<std::int64_t>(world.spacecraft().size());
    if (fleet == 0) {
        return kUnboundedStepBudget;
    }
    return std::max<std::int64_t>(1, kCraftStepsPerFrame / fleet);
}

}  // namespace omma
