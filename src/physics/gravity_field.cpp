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
        // Soften at the body's own surface: inside it the point-mass model
        // is wrong regardless, and anything there has crashed (see header).
        const double radius = bodies_[i]->meanRadius();
        minimumRadiusSquared_[i] = radius * radius;
    }
}

void GravityField::refresh(Epoch t) {
    // Memoised on the epoch: every craft in a step integrates over the same
    // four stage epochs, so without the memo that is 4N refreshes per step
    // instead of 4 (docs/DESIGN.md §5). Epoch is exact integer nanoseconds,
    // so this comparison is exact — a float epoch would miss the cache on the
    // last bit and the optimisation would silently do nothing.
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
    // The hot loop: contiguous PODs, no virtual calls, no allocation.
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
        // rSquared: avoids shadowing the free distanceSquared() (-Wshadow).
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
