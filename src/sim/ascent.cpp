#include "sim/ascent.hpp"

#include "core/units.hpp"
#include "physics/orbital_elements.hpp"
#include "sim/spacecraft.hpp"

#include <algorithm>
#include <cmath>

namespace omma {
namespace {

/// Unit vector of the launch azimuth in the local horizontal plane: north
/// rotated \p azimuth clockwise (east at pi/2), built from the local frame at
/// \p up. Degenerate at the exact poles, where "north" stops meaning much.
Vec3 downrangeDirection(const Vec3& up, double azimuth) noexcept {
    const Vec3 pole{0.0, 0.0, 1.0};
    Vec3 east = cross(pole, up).normalized();
    if (east.normSquared() < 0.5) {
        east = Vec3{0.0, 1.0, 0.0};   // launching from the pole: pick one
    }
    const Vec3 north = cross(up, east);
    return north * std::cos(azimuth) + east * std::sin(azimuth);
}

/// Command thrust along \p direction with no timed cutoff — the phase logic
/// decides when to stop, not a clock.
void thrustAlong(Spacecraft& craft, const Vec3& direction, Epoch now,
                 double throttle = 1.0) {
    craft.thrust.frame = ThrustCommand::Frame::World;
    craft.thrust.worldDirection = direction;
    craft.thrust.throttle = std::clamp(throttle, 0.0, 1.0);
    craft.thrust.cutoff = now + std::chrono::hours{24};
    craft.thrust.active = true;
}

/// Throttle that delivers \p deltaVNeeded over roughly the next second at
/// full-thrust acceleration \p maxAcceleration. Linearized terminal guidance:
/// near burnout the vehicle pulls ~190 m/s^2, so a binary cutoff sampled once
/// a second overshoots by whole steps; commanding exactly the remaining
/// impulse instead makes the cutoff accurate to the linearization error.
double throttleFor(double deltaVNeeded, double maxAcceleration) noexcept {
    if (!(maxAcceleration > 0.0)) {
        return 0.0;
    }
    return std::clamp(deltaVNeeded / maxAcceleration, 0.0, 1.0);
}

void cutEngine(Spacecraft& craft) {
    craft.thrust.active = false;
    craft.thrust.throttle = 0.0;
}

}  // namespace

void advanceAscent(Spacecraft& craft, const StateVector& relative,
                   const IEphemeris& central, Epoch now) {
    AscentProgram& program = craft.ascent;
    if (!program.active()) {
        return;
    }
    if (!craft.hasPropellant()) {
        // Ran dry mid-ascent: whatever orbit (or arc) it is on, it is done.
        cutEngine(craft);
        program.phase = AscentProgram::Phase::Done;
        return;
    }

    const double gm = central.gravitationalParameter();
    const double altitude = relative.position.norm() - central.meanRadius();
    const Vec3 up = relative.position.normalized();
    const auto elements = elementsFromState(relative, gm, now);

    switch (program.phase) {
        case AscentProgram::Phase::Vertical:
            thrustAlong(craft, up, now);
            if (altitude > program.pitchStartAltitude) {
                program.phase = AscentProgram::Phase::PitchOver;
            }
            break;

        case AscentProgram::Phase::PitchOver: {
            // Open-loop pitch schedule: 90 deg at pitchStartAltitude, 0 at
            // pitchEndAltitude, SQUARE-ROOT in altitude between — pitching
            // hard early and flattening out high, the shape of a real gravity
            // turn. The first version was linear and the flight trace showed
            // why that fails: still 45 deg nose-up at 34 km, the whole tank
            // spent against gravity, nothing left to circularize with.
            const double t = std::clamp(
                (altitude - program.pitchStartAltitude)
                    / (program.pitchEndAltitude - program.pitchStartAltitude),
                0.0, 1.0);
            const double pitch = (1.0 - std::sqrt(t)) * constants::kPi / 2.0;

            // Closed loop on the apoapsis: remaining delta-v from the
            // first-order sensitivity dApo/dv = 2 a^2 v / mu (differentiate
            // vis-viva at fixed r), commanded through the throttle so the
            // cutoff is accurate instead of quantized to whole steps.
            const double apoGap =
                program.targetApoapsisRadius - apoapsisRadius(elements);
            if (apoGap <= 1'000.0) {
                cutEngine(craft);
                program.phase = AscentProgram::Phase::Coast;
                break;
            }
            const double a = elements.semiMajorAxis;
            const double speed = relative.velocity.norm();
            const double sensitivity = 2.0 * a * a * speed / gm;   // m per (m/s)
            const double deltaVNeeded = apoGap / std::max(sensitivity, 1.0);

            const Vec3 downrange = downrangeDirection(up, program.azimuthRadians);
            const Vec3 direction =
                (up * std::sin(pitch) + downrange * std::cos(pitch)).normalized();
            thrustAlong(craft, direction, now,
                        throttleFor(deltaVNeeded, craft.maxAccelerationMps2()));
            break;
        }

        case AscentProgram::Phase::Coast:
            // Wait for the ACTUAL apoapsis — the radial velocity's sign flip.
            // Every radius-based trigger tried here fired early and lit the
            // engine while still climbing, which raises the far side instead
            // of the near one; the sign flip cannot be early by construction.
            if (dot(relative.velocity, up) < 0.0) {
                // Capture the setpoint ONCE: circularize AT this radius. A
                // target derived from the current apoapsis is a moving target
                // the burn itself moves — burning slightly past apoapsis also
                // raises apoapsis, and periapsis chased it to 12,700 km here
                // before this line existed.
                program.targetApoapsisRadius = relative.position.norm();
                program.phase = AscentProgram::Phase::Circularize;
            }
            break;

        case AscentProgram::Phase::Circularize: {
            // Drive periapsis toward the CAPTURED radius and stop on geometry
            // (roundness == eccentricity), never on a speed test, which is
            // only meaningful exactly at apoapsis. Same sensitivity as MECO.
            // Exit on the geometry (periapsis reached the setpoint); the
            // eccentricity check is only a tight safety for the exactly-
            // circular edge. At 0.005 it fired first, 44 km early: at LEO
            // radius that "round enough" still spans 50 km between apsides.
            const double circRadius = program.targetApoapsisRadius;
            if (periapsisRadius(elements) >= 0.998 * circRadius
                || elements.eccentricity < 0.0015) {
                cutEngine(craft);
                program.phase = AscentProgram::Phase::Done;
                break;
            }
            const double a = elements.semiMajorAxis;
            const double speed = relative.velocity.norm();
            const double sensitivity = 2.0 * a * a * speed / gm;
            const double periGap = 0.998 * circRadius - periapsisRadius(elements);
            const double deltaVNeeded = periGap / std::max(sensitivity, 1.0);

            // Capped low for precision: at burnout mass a full-throttle second
            // is ~185 m/s, most of this whole burn in one uncontrollable gulp
            // (measured: apoapsis 388 km on a 161 km circularization). Small
            // steps end where the geometry says, not where quantization does.
            const double throttle = std::min(
                throttleFor(deltaVNeeded, craft.maxAccelerationMps2()), 0.15);
            thrustAlong(craft, relative.velocity.normalized(), now, throttle);
            break;
        }

        case AscentProgram::Phase::None:
        case AscentProgram::Phase::Done:
            break;
    }
}

const char* ascentPhaseName(AscentProgram::Phase phase) noexcept {
    switch (phase) {
        case AscentProgram::Phase::None:        return "none";
        case AscentProgram::Phase::Vertical:    return "vertical";
        case AscentProgram::Phase::PitchOver:   return "pitch-over";
        case AscentProgram::Phase::Coast:       return "coast";
        case AscentProgram::Phase::Circularize: return "circularize";
        case AscentProgram::Phase::Done:        return "done";
    }
    return "none";
}

}  // namespace omma
