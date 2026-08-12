#include "sim/world.hpp"

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
    // NOTE ON STEP SIZE
    // One second is comfortable for low Earth orbit: the orbital period is about
    // 5500 s, so a step is ~0.07 degrees of arc and RK4's error is far below a
    // millimetre per orbit. It is NOT comfortable for a 200 km periapsis on a
    // highly eccentric orbit, where the craft covers ten times the angle per
    // second. The integrator does not adapt; that is a deliberate simplification
    // for determinism, and the price is that the scenario has to choose a step
    // appropriate to its tightest orbit.
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

        // HOW LONG IS THE ENGINE ACTUALLY ON DURING THIS STEP?
        //
        // Not necessarily the whole step. A burn cutoff falls wherever the rocket
        // equation put it, which is almost never on a step boundary — and a burn
        // can be shorter than dt outright.
        //
        // The first version ignored this: thrust was applied for the full step and
        // the cutoff was only checked afterwards. A 0.19 s burn at 200 kN
        // therefore delivered a full second of thrust — 625 m/s instead of the
        // 120 m/s requested, a factor of 5.2. It also quietly over-delivered every
        // normal burn by the length of its final partial step, which showed up as
        // a 7% error that looked like a physics subtlety and was not.
        //
        // Scaling the acceleration by the duty cycle spreads the correct impulse
        // across the step. That is a zero-order hold: the impulse is right, its
        // distribution within the step is smeared. At dt = 1 s against burns of
        // seconds to minutes the timing error is far below the integrator's own.
        const double remaining = toSeconds(craft.thrust.cutoff - stepStart);
        const double activeSeconds = std::clamp(remaining, 0.0, dt);
        const double dutyCycle = activeSeconds / dt;
        // Direction is resolved in the craft's LOCAL orbital frame. "Prograde"
        // means along the orbit around its central body, not along its
        // heliocentric velocity -- those differ by 30 km/s for anything near
        // Earth, so getting this wrong sends every burn in roughly the direction
        // Earth happens to be travelling.
        const StateVector local = relativeState(craft);
        const Vec3 direction = thrustDirection(craft.thrust, local);

        const double throttle = std::clamp(craft.thrust.throttle, 0.0, 1.0);
        const double force = craft.maxThrustNewtons * throttle;

        // Propellant flow from the thrust and the exhaust velocity: mdot = F/ve,
        // charged for the time the engine was actually on.
        const double burned = std::min(craft.propellantKg,
                                       (force / craft.exhaustVelocity) * activeSeconds);

        // MASS AT THE MIDPOINT OF THE STEP, not at its start.
        //
        // The craft gets lighter as it burns, so a step that uses the starting mass
        // throughout under-estimates the acceleration. The size of the error is
        // exactly the rocket equation's logarithm: delivered dv comes out as
        // dm/m0 instead of ve*ln(m0/m1), which is 2.7% low for a step that
        // consumes 5% of the craft's mass.
        //
        // Evaluating the mass halfway through the propellant consumed this step is
        // the midpoint rule, and it cuts that to a rounding error for one extra
        // subtraction. Worth it: a manoeuvre that silently delivers 97% of what
        // was asked for is exactly the kind of small bias that makes a rendezvous
        // impossible to close.
        const double massMidpoint = craft.totalMassKg() - 0.5 * burned;
        const double acceleration = force / massMidpoint;

        extraAcceleration = direction * (acceleration * dutyCycle);

        craft.propellantKg -= burned;
        craft.deltaVSpentMps += acceleration * activeSeconds;
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
    // GM of the pair. The craft's own mass is utterly negligible against a planet,
    // but writing it this way means the same function is correct if this is ever
    // used for a moon.
    const double gm = body.gravitationalParameter();
    return elementsFromState(relativeState(craft), gm, clock_.now());
}

double World::altitudeOf(const Spacecraft& craft) const noexcept {
    const auto& body = *system_.bodies()[craft.centralBodyIndex];
    return relativeState(craft).position.norm() - body.meanRadius();
}

void World::detectCollisions() {
    // Against celestial bodies only, for now. Craft-versus-craft needs a spatial
    // index to stay affordable at thousands of objects, and that arrives with the
    // constellation work.
    //
    // Note this is a POSITION test at the end of a step, not a swept test. A craft
    // moving 7.7 km/s covers 7.7 km in a one-second step, so it can in principle
    // pass through a thin shell. It cannot pass through a planet, which is what
    // this is for -- but the limitation is real and is why the check belongs at
    // the end of the step rather than the beginning.
    gravity_.refresh(clock_.now());

    for (Spacecraft& craft : spacecraft_) {
        if (!craft.isAlive()) {
            continue;
        }
        for (std::size_t i = 0; i < system_.size(); ++i) {
            const auto& body = *system_.bodies()[i];
            const double distanceToCentre =
                distance(craft.state.position, body.sample(clock_.now()).position);
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

    // Altitude is specified at PERIAPSIS, which is the useful convention: it is
    // the number that determines whether you survive, and "400 km orbit" with
    // eccentricity always means 400 km at the low point.
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

}  // namespace omma
