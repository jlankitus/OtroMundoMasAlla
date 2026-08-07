// ─────────────────────────────────────────────────────────────────────────────
// SolarSystem — the standard set of bodies, wired up with their real data.
//
// Owns the ephemerides and guarantees that a parent outlives its children, so
// the non-owning parent pointers inside KeplerEphemeris are always valid.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "physics/kepler_ephemeris.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace omma {

enum class BodyId : std::size_t {
    Sun = 0,
    Mercury,
    Venus,
    Earth,
    Moon,
    Mars,
    Jupiter,
    Saturn,
    Uranus,
    Neptune,
    Pluto,
    Count
};

[[nodiscard]] std::string_view bodyName(BodyId id) noexcept;

class SolarSystem {
public:
    /// The Sun, eight planets, the Moon, and Pluto because it is nice to have
    /// somewhere far away to fly to.
    [[nodiscard]] static SolarSystem standard();

    SolarSystem(SolarSystem&&) noexcept = default;
    SolarSystem& operator=(SolarSystem&&) noexcept = default;
    SolarSystem(const SolarSystem&) = delete;
    SolarSystem& operator=(const SolarSystem&) = delete;

    [[nodiscard]] const IEphemeris& operator[](BodyId id) const noexcept;
    [[nodiscard]] const IEphemeris* findByName(std::string_view name) const noexcept;

    /// Bodies in declaration order, parents always before their children.
    [[nodiscard]] const std::vector<std::unique_ptr<IEphemeris>>& bodies() const noexcept {
        return bodies_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return bodies_.size(); }

    /// The body every root orbit is measured against.
    [[nodiscard]] const IEphemeris& centralBody() const noexcept { return *bodies_.front(); }

private:
    SolarSystem() = default;

    std::vector<std::unique_ptr<IEphemeris>> bodies_;
};

}  // namespace omma
