// Ascent guidance: how a spacecraft gets from a launch pad to orbit.
//
// The design choice worth defending: guidance is a STRATEGY that writes to
// the same ThrustCommand actuator the player's burn keys use. The autopilot
// and the human share one control path into one physics path, so gravity
// losses, drag losses and the rotation of the Earth are not simulated for
// the ascent — they simply happen to it, through the same integrator every
// coasting satellite uses.
//
// The program itself is the classic pre-PEG shape real boosters flew:
//     1. Vertical  — rise clear of the pad (open loop)
//     2. PitchOver — pitch from up toward downrange, scheduled on altitude
//                    (open loop; the "gravity turn" approximation)
//     3. Coast     — engine off once the apoapsis reaches target (closed loop)
//     4. Circularize — burn prograde at apoapsis until periapsis rises
//                    (closed loop)
// Open-loop where a schedule is cheap and adequate, closed-loop where the
// orbit actually matters — the split is the same one flight software makes.
#pragma once

#include "physics/ephemeris.hpp"
#include "physics/state_vector.hpp"

#include <cstdint>

namespace omma {

struct Spacecraft;

/// Where and how to lift off. Defaults describe a small orbital rocket with
/// sensible mass ratio; the thrust-to-weight at the pad is ~1.5.
struct SurfaceLaunchRequest {
    double latitudeRadians{0.4974};    ///< 28.5 N: Cape Canaveral
    double longitudeRadians{-1.4075};  ///< 80.6 W
    double targetAltitudeMetres{200.0e3};
    /// Launch azimuth, radians clockwise from north. pi/2 (due east) takes
    /// the full rotation bonus and yields inclination == latitude.
    double azimuthRadians{1.5708};

    double dryMassKg{500.0};
    double propellantKg{9500.0};
    double maxThrustNewtons{150.0e3};
    double exhaustVelocityMps{3500.0};
    /// A rocket is pointy: Cd ~0.5 on ~2 m^2, not the satellite default of
    /// 2.2 on a tumbling body. At max-q the difference is the whole flight.
    double dragCoefficient{0.5};
    double crossSectionM2{2.0};
};

/// The ascent autopilot's state machine, carried on the spacecraft.
struct AscentProgram {
    enum class Phase : std::uint8_t {
        None,         ///< not an ascending vehicle
        Vertical,
        PitchOver,
        Coast,
        Circularize,
        Done,
    };

    Phase  phase{Phase::None};
    double targetApoapsisRadius{0.0};   ///< metres from body centre
    double azimuthRadians{0.0};

    /// Pitch schedule bounds: straight up below the first, horizontal above
    /// the second, square-root-shaped between. Metres of altitude.
    double pitchStartAltitude{1.0e3};
    double pitchEndAltitude{65.0e3};

    [[nodiscard]] bool active() const noexcept {
        return phase != Phase::None && phase != Phase::Done;
    }
};

/// Advance the autopilot one step: read the craft's state relative to its
/// central body, decide a phase, and write the engine command. Pure guidance —
/// it never touches position, velocity, or propellant; the integrator does.
void advanceAscent(Spacecraft& craft, const StateVector& relative,
                   const IEphemeris& central, Epoch now);

[[nodiscard]] const char* ascentPhaseName(AscentProgram::Phase phase) noexcept;

}  // namespace omma
