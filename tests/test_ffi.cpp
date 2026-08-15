// The C ABI, tested through the C surface only — the way an engine sees it.
// If a test here needs a C++ header from the sim, the ABI is leaking.

#include "ffi/omma_c.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

struct SimHandle {
    OmmaSim* sim;
    explicit SimHandle(const char* scenario, double startDays = 9350.0)
        : sim{omma_create(scenario, startDays)} {}
    ~SimHandle() { omma_destroy(sim); }
    SimHandle(const SimHandle&) = delete;
    SimHandle& operator=(const SimHandle&) = delete;
};

}  // namespace

TEST_CASE("the ABI version is exported", "[ffi]") {
    REQUIRE(omma_abi_version() == OMMA_ABI_VERSION);
}

TEST_CASE("an unknown scenario returns NULL, not a crash", "[ffi]") {
    REQUIRE(omma_create("marsbase", 0.0) == nullptr);
}

TEST_CASE("leo opens with eleven bodies and three craft", "[ffi]") {
    SimHandle h{"leo"};
    REQUIRE(h.sim != nullptr);
    REQUIRE(omma_body_count(h.sim) == 11);
    REQUIRE(omma_craft_count(h.sim) == 3);

    char name[32];
    omma_craft_name(h.sim, 0, name, sizeof(name));
    REQUIRE(std::string{name} == "ISS-LIKE");

    OmmaCraftState state{};
    REQUIRE(omma_craft_state(h.sim, 0, &state) == 1);
    REQUIRE(state.alive == 1);
    // Heliocentric position: on Earth's orbit, so about 1 au from the origin.
    const double r = std::sqrt(state.position.x * state.position.x
                               + state.position.y * state.position.y
                               + state.position.z * state.position.z);
    REQUIRE_THAT(r, WithinRel(1.496e11, 0.05));
}

TEST_CASE("advance is the pacer: whole steps, remainder carried", "[ffi]") {
    SimHandle h{"empty"};
    const double before = omma_now_seconds_since_j2000(h.sim);

    // 60 frames at 1/30 s, warp 60: exactly 120 one-second steps in total.
    std::int64_t total = 0;
    for (int i = 0; i < 60; ++i) {
        total += omma_advance(h.sim, 1.0 / 30.0, 60.0);
    }
    REQUIRE(total == 120);
    REQUIRE_THAT(omma_now_seconds_since_j2000(h.sim) - before, WithinAbs(120.0, 1e-9));
    REQUIRE(omma_falling_behind(h.sim) == 0);
}

TEST_CASE("launch, burn, and honest delta-v accounting", "[ffi]") {
    SimHandle h{"empty"};
    REQUIRE(omma_craft_count(h.sim) == 0);

    OmmaLaunchRequest request{};
    request.name = "PROBE";
    request.aroundBodyIndex = 3;   // Earth
    request.altitudeMetres = 400.0e3;
    request.inclinationRad = 0.9;
    request.dryMassKg = 200.0;
    request.propellantKg = 80.0;
    request.maxThrustNewtons = 400.0;
    request.exhaustVelocityMps = 2200.0;

    const std::int64_t id = omma_launch(h.sim, &request);
    REQUIRE(id != 0);
    REQUIRE(omma_craft_count(h.sim) == 1);
    REQUIRE(omma_craft_id(h.sim, 0) == id);

    OmmaElements before{};
    REQUIRE(omma_craft_elements(h.sim, 0, &before) == 1);
    const double apoBefore = before.apoapsisRadius;

    REQUIRE(omma_command_delta_v(h.sim, id, OMMA_BURN_PROGRADE, 50.0) == 1);
    omma_step(h.sim, 600);   // ten minutes: burn completes

    OmmaCraftState state{};
    REQUIRE(omma_craft_state(h.sim, 0, &state) == 1);
    REQUIRE_THAT(state.deltaVSpentMps, WithinRel(50.0, 1e-3));

    OmmaElements after{};
    REQUIRE(omma_craft_elements(h.sim, 0, &after) == 1);
    REQUIRE(after.apoapsisRadius > apoBefore + 50.0e3);         // apo climbed
    REQUIRE_THAT(after.periapsisRadius, WithinRel(before.periapsisRadius, 0.01));
}

TEST_CASE("a burn a craft cannot afford is refused", "[ffi]") {
    SimHandle h{"leo"};
    const std::int64_t id = omma_craft_id(h.sim, 0);
    REQUIRE(omma_command_delta_v(h.sim, id, OMMA_BURN_PROGRADE, 1.0e6) == 0);
}

TEST_CASE("orbit polylines are finite, closed and on-scale", "[ffi]") {
    SimHandle h{"leo"};

    constexpr int kPoints = 64;
    std::vector<double> xyz(static_cast<std::size_t>(kPoints) * 3);

    // Earth's heliocentric orbit.
    REQUIRE(omma_body_orbit(h.sim, 3, xyz.data(), kPoints) == kPoints);
    for (const double v : xyz) {
        REQUIRE(std::isfinite(v));
    }
    // First and last sample coincide: E = 0 and E = 2 pi.
    REQUIRE_THAT(xyz[0], WithinAbs(xyz[(kPoints - 1) * 3], 1.0));

    // A craft's orbit hugs Earth: every point within ~8000 km of Earth's centre.
    OmmaBodyState earth{};
    REQUIRE(omma_body_state(h.sim, 3, &earth) == 1);
    REQUIRE(omma_craft_orbit(h.sim, 0, xyz.data(), kPoints) == kPoints);
    for (int i = 0; i < kPoints; ++i) {
        const double dx = xyz[i * 3 + 0] - earth.position.x;
        const double dy = xyz[i * 3 + 1] - earth.position.y;
        const double dz = xyz[i * 3 + 2] - earth.position.z;
        REQUIRE(std::sqrt(dx * dx + dy * dy + dz * dz) < 8.0e6);
    }

    // The Sun has no orbit, and a bad index is a 0, not a crash.
    REQUIRE(omma_body_orbit(h.sim, 0, xyz.data(), kPoints) == 0);
    REQUIRE(omma_craft_orbit(h.sim, 99, xyz.data(), kPoints) == 0);
}

TEST_CASE("bodies report parentage for frame-relative rendering", "[ffi]") {
    SimHandle h{"empty"};
    OmmaBodyState moon{};
    REQUIRE(omma_body_state(h.sim, 4, &moon) == 1);   // Moon orbits Earth (3)
    REQUIRE(moon.parentIndex == 3);
    OmmaBodyState sun{};
    REQUIRE(omma_body_state(h.sim, 0, &sun) == 1);
    REQUIRE(sun.parentIndex == -1);
}
