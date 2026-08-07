// ─────────────────────────────────────────────────────────────────────────────
// GravityField — what the environment does to a vehicle.
//
// PUT POLYMORPHISM WHERE N IS SMALL, DATA LAYOUT WHERE N IS LARGE
//
// The naive design calls IEphemeris::sample(t) inside the per-spacecraft
// acceleration loop. With eleven bodies, four RK4 stages and ten thousand
// spacecraft that is 440,000 virtual calls per step, each one re-solving
// Kepler's equation for a planet whose position has not changed.
//
// Instead the field is refreshed ONCE per integrator stage: eleven virtual
// calls that flatten every source into a contiguous array of
// {position, GM} PODs. The spacecraft loop then runs over that array with no
// virtual dispatch, no branching and no pointer chasing.
//
//     refresh(t)        11 virtual calls, once per stage
//     accelerationAt()  tight loop over contiguous PODs, once per spacecraft
//
// Notice the interface sits at the eleven-planets boundary, not the
// ten-thousand-satellites boundary. Most "OOP is slow" pain comes from getting
// that the wrong way round.
//
// TWO-PHASE API, AND WHY THAT IS SAFE HERE
// refresh-then-query is a shape that invites bugs: forget the refresh and you
// silently integrate against last stage's planets. It survives because the only
// code that touches it is integrate(), which owns both halves and is itself
// tested. Callers of the integrator never see the two phases. When an API is
// easy to misuse, confine the use to one place and test that place hard.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/epoch.hpp"
#include "physics/ephemeris.hpp"
#include "physics/state_vector.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace omma {

/// One gravity source, frozen at an instant. Deliberately a flat POD: this is
/// the type the inner loop reads, so it wants to be small, trivially copyable
/// and free of anything the compiler cannot see through.
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
    /// Softening: a point mass has infinite acceleration at zero distance, and
    /// a spacecraft that passes exactly through a body's centre would produce
    /// inf, then NaN, then a permanently dead run. Distances below the body's
    /// own radius are not physical anyway — you have crashed — so the
    /// denominator is floored. The collision is detected elsewhere; this just
    /// makes sure the numbers stay finite until it is.
    [[nodiscard]] Vec3 accelerationAt(const Vec3& position) const noexcept;

    /// Index of the source with the strongest pull at \p position, or
    /// npos when the field has no sources.
    ///
    /// This is what a sphere-of-influence test reduces to. Needed later to
    /// decide which body a spacecraft's orbital elements should be measured
    /// against, and which ellipse to freeze it onto when it goes on rails.
    [[nodiscard]] std::size_t dominantSourceIndex(const Vec3& position) const noexcept;

    [[nodiscard]] std::span<const GravitySource> sources() const noexcept { return cache_; }
    [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
    [[nodiscard]] const IEphemeris& body(std::size_t index) const noexcept {
        return *bodies_[index];
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    std::vector<const IEphemeris*> bodies_;
    std::vector<GravitySource>     cache_;
    std::vector<double>            minimumRadiusSquared_;
    Epoch                          sampledAt_{};
    bool                           refreshed_{false};
};

/// Build a field from every body in a solar system. The common case.
class SolarSystem;
[[nodiscard]] GravityField makeGravityField(const SolarSystem& system);

}  // namespace omma
