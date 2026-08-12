#include "physics/gravity_field.hpp"

#include "physics/solar_system.hpp"

#include <cmath>
#include <utility>

namespace omma {

GravityField::GravityField(std::vector<const IEphemeris*> sources)
    : bodies_{std::move(sources)} {
    cache_.resize(bodies_.size());
    minimumRadiusSquared_.resize(bodies_.size());
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        // Soften at the body's own surface. Inside a body the point-mass model
        // is wrong regardless (the enclosed mass drops as r^3), and anything
        // that gets there has crashed. This only keeps the arithmetic finite
        // long enough for the collision check to notice.
        const double radius = bodies_[i]->meanRadius();
        minimumRadiusSquared_[i] = radius * radius;
    }
}

void GravityField::refresh(Epoch t) {
    // MEMOISED ON THE EPOCH, and this is not a micro-optimisation.
    //
    // Every spacecraft in a step is integrated over the SAME four stage epochs, so
    // a naive per-craft integrate() call re-samples the entire solar system four
    // times per craft: 4N refreshes per step instead of 4. Each refresh solves
    // Kepler's equation for eleven bodies, so with three craft that is 132 Kepler
    // solves per step where twelve would do.
    //
    // Measured before the cache: 76 ms per frame at one simulated day per second
    // with three satellites -- 13 fps against a 30 fps target, with the integrator
    // dominating. The whole point of flattening the sources into a POD array was
    // to stop paying per-craft costs for per-environment work, and calling refresh
    // from inside the per-craft loop quietly undid it.
    //
    // Epoch is exact integer nanoseconds, so this comparison is exact. That
    // matters: a float epoch would miss the cache on the last bit and the
    // optimisation would silently do nothing.
    if (refreshed_ && sampledAt_ == t) {
        return;
    }

    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        cache_[i] = GravitySource{bodies_[i]->sample(t).position,
                                  bodies_[i]->gravitationalParameter()};
    }
    sampledAt_ = t;
    refreshed_ = true;
}

Vec3 GravityField::accelerationAt(const Vec3& position) const noexcept {
    // The hot loop. Contiguous PODs, no virtual calls, no allocation, nothing
    // the optimiser cannot see through.
    Vec3 acceleration{};

    for (std::size_t i = 0; i < cache_.size(); ++i) {
        const GravitySource& source = cache_[i];
        const Vec3 offset{source.position.x - position.x,
                          source.position.y - position.y,
                          source.position.z - position.z};

        double distanceSquared = offset.normSquared();
        if (distanceSquared < minimumRadiusSquared_[i]) {
            distanceSquared = minimumRadiusSquared_[i];
        }
        if (distanceSquared <= 0.0) {
            continue;
        }

        // a = GM * d / |d|^3. One sqrt, then a reciprocal cube — cheaper and
        // more accurate than normalising and dividing separately.
        const double distance = std::sqrt(distanceSquared);
        const double scale = source.gm / (distanceSquared * distance);
        acceleration.x += offset.x * scale;
        acceleration.y += offset.y * scale;
        acceleration.z += offset.z * scale;
    }

    return acceleration;
}

std::size_t GravityField::dominantSourceIndex(const Vec3& position) const noexcept {
    std::size_t best = npos;
    double strongest = 0.0;

    for (std::size_t i = 0; i < cache_.size(); ++i) {
        const GravitySource& source = cache_[i];
        // Named separately from the free distanceSquared() to avoid shadowing
        // it — -Wshadow is on precisely because a shadowed name in a physics
        // kernel is a silent accuracy bug rather than a style nit.
        double rSquared = distanceSquared(source.position, position);
        if (rSquared < minimumRadiusSquared_[i]) {
            rSquared = minimumRadiusSquared_[i];
        }
        if (rSquared <= 0.0) {
            return i;
        }
        const double pull = source.gm / rSquared;
        if (pull > strongest) {
            strongest = pull;
            best = i;
        }
    }
    return best;
}

GravityField makeGravityField(const SolarSystem& system) {
    std::vector<const IEphemeris*> sources;
    sources.reserve(system.size());
    for (const auto& body : system.bodies()) {
        sources.push_back(body.get());
    }
    return GravityField{std::move(sources)};
}

}  // namespace omma
