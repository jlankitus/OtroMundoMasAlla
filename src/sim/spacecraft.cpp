#include "sim/spacecraft.hpp"

#include <cmath>

namespace omma {

double Spacecraft::remainingDeltaVMps() const noexcept {
    const double wet = totalMassKg();
    const double dry = dryMassKg;
    if (!(dry > 0.0) || wet <= dry) {
        return 0.0;
    }
    return exhaustVelocity * std::log(wet / dry);
}

Vec3 thrustDirection(const ThrustCommand& command, const StateVector& state) noexcept {
    using Frame = ThrustCommand::Frame;

    switch (command.frame) {
        case Frame::World:
            return command.worldDirection.normalized();

        case Frame::Prograde:
            return state.velocity.normalized();
        case Frame::Retrograde:
            return -state.velocity.normalized();

        case Frame::Normal:
            // Along the specific angular momentum, r x v. This is the axis the
            // orbit rotates about, so pushing along it tilts the orbital plane
            // without changing its size -- the only way to change inclination.
            return cross(state.position, state.velocity).normalized();
        case Frame::AntiNormal:
            return -cross(state.position, state.velocity).normalized();

        case Frame::RadialOut:
            return state.position.normalized();
        case Frame::RadialIn:
            return -state.position.normalized();
    }
    return Vec3::zero();
}

}  // namespace omma
