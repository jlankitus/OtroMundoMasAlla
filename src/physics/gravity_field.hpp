// ─────────────────────────────────────────────────────────────────────────────
// GravityField — what the environment does to a vehicle.
//
// Polymorphism where N is small, data layout where N is large: refresh(t)
// makes eleven virtual calls once per integrator stage, flattening every
// source into a contiguous array of {position, GM} PODs; accelerationAt()
// then runs a tight per-spacecraft loop with no virtual dispatch. The
// two-phase refresh-then-query API is confined to integrate(), which owns
// both halves and is tested. See docs/DESIGN.md §5.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/ephemeris.hpp"
#include "physics/state_vector.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace omma {

/// One gravity source, frozen at an instant. Deliberately a flat POD: the
/// inner loop reads it, so it stays small, trivially copyable, transparent.
struct GravitySource {
    Vec3   position;
    double gm{0.0};
};

class GravityField {
public:
    /// \param sources  non-owning pointers to the bodies whose gravity matters.
    ///                 Must outlive the field. SolarSystem owns them.
    explicit GravityField(std::vector<const IEphemeris*> sources);

    /// Freeze every source's position at \p t. Call once per integrator stage.
    void refresh(Epoch t);

    /// The instant the cache currently represents.
    [[nodiscard]] Epoch sampledAt() const noexcept { return sampledAt_; }
    [[nodiscard]] bool isRefreshed() const noexcept { return refreshed_; }

    /// Newtonian acceleration at \p position, from the cached sources.
    ///
    /// Softening: distances are floored at each body's own radius. Below that
    /// you have crashed; this only keeps the arithmetic finite until the
    /// collision check (which lives elsewhere) notices.
    [[nodiscard]] Vec3 accelerationAt(const Vec3& position) const noexcept;

    /// Index of the source with the strongest pull at \p position, or npos
    /// when the field has no sources. A sphere-of-influence test: decides
    /// which body a craft's elements are measured against, and which ellipse
    /// it freezes onto when it goes on rails.
    [[nodiscard]] std::size_t dominantSourceIndex(const Vec3& position) const noexcept;

    [[nodiscard]] std::span<const GravitySource> sources() const noexcept { return cache_; }

    /// Cached position of one source. Use instead of re-calling sample():
    /// a sample costs a Kepler solve, the cache costs a load.
    [[nodiscard]] const Vec3& positionOf(std::size_t index) const noexcept {
        return cache_[index].position;
    }
    [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
    [[nodiscard]] const IEphemeris& body(std::size_t index) const noexcept {
        return *bodies_[index];
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    std::vector<const IEphemeris*> bodies_;
    std::vector<GravitySource>     cache_;
    std::vector<double>            minimumRadiusSquared_;
    std::vector<double>            j2_;
    std::vector<double>            equatorialRadiusSquared_;
    Epoch                          sampledAt_{};
    bool                           refreshed_{false};
};

/// Build a field from every body in a solar system. The common case.
class SolarSystem;
[[nodiscard]] GravityField makeGravityField(const SolarSystem& system);

}  // namespace omma
