// The C ABI over the C++ sim. Three rules hold everywhere in this file:
// no exception escapes the boundary (bad input returns 0/NULL), every struct
// crossing it is a POD from omma_c.h, and nothing here computes physics —
// it only forwards to sim/physics, so the ABI cannot drift from the sim.
#include "ffi/omma_c.h"

#include "core/epoch.hpp"
#include "core/sim_clock.hpp"
#include "core/units.hpp"
#include "physics/kepler_ephemeris.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/solar_system.hpp"
#include "sim/scenario.hpp"
#include "sim/transfer.hpp"
#include "sim/world.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

using namespace omma;

/// The handle: a world plus the same pacing state an interactive client keeps.
struct OmmaSim {
    World     world;
    StepPacer pacer;
    bool      clamped{false};

    OmmaSim(World w)
        : world{std::move(w)}, pacer{std::chrono::seconds{1}, kUnboundedStepBudget} {}
};

namespace {

OmmaVec3 toC(const Vec3& v) { return OmmaVec3{v.x, v.y, v.z}; }

SpacecraftId unpackId(int64_t packed) {
    return SpacecraftId{static_cast<std::uint32_t>(packed & 0xFFFF'FFFF),
                        static_cast<std::uint32_t>(static_cast<std::uint64_t>(packed) >> 32)};
}

int64_t packId(SpacecraftId id) {
    if (!id.isValid()) {
        return 0;
    }
    return static_cast<int64_t>((static_cast<std::uint64_t>(id.generation) << 32)
                                | id.index);
}

int32_t copyName(std::string_view name, char* buf, int32_t cap) {
    if (buf != nullptr && cap > 0) {
        const auto n = std::min<std::size_t>(name.size(),
                                             static_cast<std::size_t>(cap) - 1);
        std::memcpy(buf, name.data(), n);
        buf[n] = '\0';
    }
    return static_cast<int32_t>(name.size());
}

const Spacecraft* craftAt(const OmmaSim* sim, int32_t index) {
    if (sim == nullptr || index < 0
        || static_cast<std::size_t>(index) >= sim->world.spacecraft().size()) {
        return nullptr;
    }
    return &sim->world.spacecraft()[static_cast<std::size_t>(index)];
}

/// Sample a closed ellipse around `origin` — the same uniform-eccentric-
/// anomaly spacing the terminal renderer draws, via the shared basis.
int32_t sampleEllipse(const OrbitalElements& elements, const Vec3& origin,
                      double* xyz, int32_t count) {
    const double a = elements.semiMajorAxis;
    const double e = elements.eccentricity;
    if (xyz == nullptr || count < 2 || !(a > 0.0) || e >= 1.0) {
        return 0;
    }
    const auto [p, q] = perifocalBasis(elements);
    const double b = a * std::sqrt(1.0 - e * e);

    for (int32_t i = 0; i < count; ++i) {
        const double E = constants::kTwoPi * static_cast<double>(i)
                       / static_cast<double>(count - 1);
        const Vec3 point = origin + p * (a * (std::cos(E) - e)) + q * (b * std::sin(E));
        xyz[i * 3 + 0] = point.x;
        xyz[i * 3 + 1] = point.y;
        xyz[i * 3 + 2] = point.z;
    }
    return count;
}

}  // namespace

extern "C" {

int32_t omma_abi_version(void) { return OMMA_ABI_VERSION; }

OmmaSim* omma_create(const char* scenario, double startDays) {
    const auto parsed = scenarioFromName(scenario == nullptr ? "leo" : scenario);
    if (!parsed.has_value()) {
        return nullptr;
    }
    const Epoch start = Epoch::j2000()
        + std::chrono::duration_cast<Duration>(
              std::chrono::duration<double>{startDays * constants::kSecondsPerDay});

    auto* sim = new OmmaSim{World{SolarSystem::standard(),
                                  std::chrono::seconds{1}, start}};
    applyScenario(sim->world, *parsed);
    return sim;
}

void omma_destroy(OmmaSim* sim) { delete sim; }

int64_t omma_advance(OmmaSim* sim, double frameDtSeconds, double warp) {
    if (sim == nullptr || !(frameDtSeconds >= 0.0)) {
        return 0;
    }
    sim->pacer.setMaxStepsPerFrame(interactiveStepBudget(sim->world));
    const std::int64_t steps = sim->pacer.stepsForFrame(frameDtSeconds, warp);
    sim->world.step(steps);
    sim->clamped = sim->pacer.lastFrameWasClamped();
    static_cast<void>(sim->world.drainEvents());
    return steps;
}

void omma_step(OmmaSim* sim, int64_t n) {
    if (sim != nullptr && n > 0) {
        sim->world.step(n);
        static_cast<void>(sim->world.drainEvents());
    }
}

int32_t omma_falling_behind(const OmmaSim* sim) {
    return sim != nullptr && sim->clamped ? 1 : 0;
}

double omma_now_seconds_since_j2000(const OmmaSim* sim) {
    return sim == nullptr ? 0.0 : sim->world.now().secondsSinceJ2000();
}

int32_t omma_body_count(const OmmaSim* sim) {
    return sim == nullptr ? 0 : static_cast<int32_t>(sim->world.system().size());
}

int32_t omma_body_state(const OmmaSim* sim, int32_t index, OmmaBodyState* out) {
    if (sim == nullptr || out == nullptr || index < 0
        || static_cast<std::size_t>(index) >= sim->world.system().size()) {
        return 0;
    }
    const auto& body = *sim->world.system().bodies()[static_cast<std::size_t>(index)];
    const auto state = body.sample(sim->world.now());
    out->position = toC(state.position);
    out->velocity = toC(state.velocity);
    out->radius = body.meanRadius();
    out->gm = body.gravitationalParameter();

    out->parentIndex = -1;
    if (const auto* kepler = dynamic_cast<const KeplerEphemeris*>(&body);
        kepler != nullptr && kepler->parent() != nullptr) {
        const auto& bodies = sim->world.system().bodies();
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].get() == kepler->parent()) {
                out->parentIndex = static_cast<int32_t>(i);
                break;
            }
        }
    }
    return 1;
}

int32_t omma_body_name(const OmmaSim* sim, int32_t index, char* buf, int32_t cap) {
    if (sim == nullptr || index < 0
        || static_cast<std::size_t>(index) >= sim->world.system().size()) {
        return copyName("", buf, cap);
    }
    return copyName(
        sim->world.system().bodies()[static_cast<std::size_t>(index)]->name(),
        buf, cap);
}

int32_t omma_craft_count(const OmmaSim* sim) {
    return sim == nullptr ? 0
                          : static_cast<int32_t>(sim->world.spacecraft().size());
}

int32_t omma_craft_state(const OmmaSim* sim, int32_t index, OmmaCraftState* out) {
    const Spacecraft* craft = craftAt(sim, index);
    if (craft == nullptr || out == nullptr) {
        return 0;
    }
    out->position = toC(craft->state.position);
    out->velocity = toC(craft->state.velocity);
    out->centralBodyIndex = static_cast<int32_t>(craft->centralBodyIndex);
    out->alive = craft->isAlive() ? 1 : 0;
    out->burning = craft->thrust.active && craft->thrust.throttle > 0.0 ? 1 : 0;
    out->propellantKg = craft->propellantKg;
    out->deltaVLeftMps = craft->remainingDeltaVMps();
    out->deltaVSpentMps = craft->deltaVSpentMps;
    return 1;
}

int32_t omma_craft_name(const OmmaSim* sim, int32_t index, char* buf, int32_t cap) {
    const Spacecraft* craft = craftAt(sim, index);
    return copyName(craft == nullptr ? std::string_view{} : craft->name, buf, cap);
}

int32_t omma_craft_elements(const OmmaSim* sim, int32_t index, OmmaElements* out) {
    const Spacecraft* craft = craftAt(sim, index);
    if (craft == nullptr || out == nullptr) {
        return 0;
    }
    const auto elements = sim->world.elementsOf(*craft);
    const double gm = sim->world.system()
                          .bodies()[craft->centralBodyIndex]
                          ->gravitationalParameter();
    out->semiMajorAxis = elements.semiMajorAxis;
    out->eccentricity = elements.eccentricity;
    out->inclination = elements.inclination;
    out->longitudeOfAscendingNode = elements.longitudeOfAscendingNode;
    out->argumentOfPeriapsis = elements.argumentOfPeriapsis;
    out->periodSeconds = orbitalPeriod(elements, gm);
    out->periapsisRadius = periapsisRadius(elements);
    out->apoapsisRadius = apoapsisRadius(elements);
    return 1;
}

int64_t omma_launch(OmmaSim* sim, const OmmaLaunchRequest* request) {
    if (sim == nullptr || request == nullptr || request->aroundBodyIndex < 0
        || static_cast<std::size_t>(request->aroundBodyIndex)
               >= sim->world.system().size()) {
        return 0;
    }
    LaunchRequest launch{};
    launch.name = request->name != nullptr ? request->name : "SAT";
    launch.aroundBody = static_cast<BodyId>(request->aroundBodyIndex);
    launch.altitudeMetres = request->altitudeMetres;
    launch.eccentricity = request->eccentricity;
    launch.inclinationRadians = request->inclinationRad;
    launch.longitudeOfAscendingNodeRadians = request->raanRad;
    launch.argumentOfPeriapsisRadians = request->argPeriapsisRad;
    launch.meanAnomalyRadians = request->meanAnomalyRad;
    launch.dryMassKg = request->dryMassKg;
    launch.propellantKg = request->propellantKg;
    launch.maxThrustNewtons = request->maxThrustNewtons;
    launch.exhaustVelocity = request->exhaustVelocityMps;
    return packId(sim->world.launch(launch));
}

int64_t omma_launch_ascent(OmmaSim* sim, const OmmaSurfaceLaunchRequest* request) {
    if (sim == nullptr) {
        return 0;
    }
    SurfaceLaunchRequest launch{};   // the default rocket
    int32_t bodyIndex = 3;           // Earth
    const char* name = "ASCENT";
    if (request != nullptr) {
        bodyIndex = request->bodyIndex;
        if (request->name != nullptr) name = request->name;
        launch.latitudeRadians = request->latitudeRad;
        launch.longitudeRadians = request->longitudeRad;
        if (request->targetAltitudeMetres > 0.0)
            launch.targetAltitudeMetres = request->targetAltitudeMetres;
        launch.azimuthRadians = request->azimuthRad;
        if (request->dryMassKg > 0.0) launch.dryMassKg = request->dryMassKg;
        if (request->propellantKg > 0.0) launch.propellantKg = request->propellantKg;
        if (request->maxThrustNewtons > 0.0)
            launch.maxThrustNewtons = request->maxThrustNewtons;
        if (request->exhaustVelocityMps > 0.0)
            launch.exhaustVelocityMps = request->exhaustVelocityMps;
    }
    if (bodyIndex < 0
        || static_cast<std::size_t>(bodyIndex) >= sim->world.system().size()) {
        return 0;
    }
    return packId(sim->world.launchFromSurface(static_cast<BodyId>(bodyIndex),
                                               launch, name));
}

int32_t omma_craft_ascent_phase(const OmmaSim* sim, int32_t index) {
    const Spacecraft* craft = craftAt(sim, index);
    if (craft == nullptr) {
        return -1;
    }
    return static_cast<int32_t>(craft->ascent.phase);
}

int32_t omma_plan_transfer(const OmmaSim* sim, int32_t fromBody, int32_t toBody,
                           double parkingRadiusMetres, OmmaTransferPlan* out) {
    if (sim == nullptr || out == nullptr || fromBody < 0 || toBody < 0
        || static_cast<std::size_t>(fromBody) >= sim->world.system().size()
        || static_cast<std::size_t>(toBody) >= sim->world.system().size()) {
        return 0;
    }
    const TransferPlan plan = planHohmannTransfer(
        sim->world.system(), static_cast<BodyId>(fromBody),
        static_cast<BodyId>(toBody), sim->world.now(), parkingRadiusMetres);
    out->valid = plan.valid ? 1 : 0;
    out->waitSeconds = plan.waitSeconds;
    out->transferSeconds = plan.transferSeconds;
    out->phaseAngleDeg = plan.phaseAngleDeg;
    out->currentPhaseAngleDeg = plan.currentPhaseAngleDeg;
    out->vInfinityMps = plan.vInfinityMps;
    out->departureDeltaVMps = plan.departureDeltaVMps;
    return out->valid;
}

int64_t omma_craft_id(const OmmaSim* sim, int32_t index) {
    const Spacecraft* craft = craftAt(sim, index);
    return craft == nullptr ? 0 : packId(craft->id);
}

int32_t omma_command_delta_v(OmmaSim* sim, int64_t craftId, int32_t frame,
                             double deltaVMps) {
    if (sim == nullptr || frame < 0
        || frame > static_cast<int32_t>(ThrustCommand::Frame::AntiNormal)) {
        return 0;
    }
    return sim->world.commandDeltaV(unpackId(craftId),
                                    static_cast<ThrustCommand::Frame>(frame),
                                    deltaVMps)
               ? 1
               : 0;
}

void omma_cancel_burn(OmmaSim* sim, int64_t craftId) {
    if (sim != nullptr) {
        sim->world.cancelBurn(unpackId(craftId));
    }
}

int32_t omma_body_orbit(const OmmaSim* sim, int32_t index, double* xyz,
                        int32_t count) {
    if (sim == nullptr || index < 0
        || static_cast<std::size_t>(index) >= sim->world.system().size()) {
        return 0;
    }
    const auto* kepler = dynamic_cast<const KeplerEphemeris*>(
        sim->world.system().bodies()[static_cast<std::size_t>(index)].get());
    if (kepler == nullptr) {
        return 0;   // the Sun: fixed, no orbit to draw
    }
    const Epoch t = sim->world.now();
    const Vec3 origin = kepler->parent() != nullptr
                            ? kepler->parent()->sample(t).position
                            : Vec3::zero();
    return sampleEllipse(kepler->elementsAt(t), origin, xyz, count);
}

int32_t omma_craft_ground_track(const OmmaSim* sim, int32_t index,
                                double* latitudeRad, double* longitudeRad) {
    const Spacecraft* craft = craftAt(sim, index);
    if (craft == nullptr || latitudeRad == nullptr || longitudeRad == nullptr) {
        return 0;
    }
    const LatLon track = sim->world.groundTrackOf(*craft);
    *latitudeRad = track.latitudeRadians;
    *longitudeRad = track.longitudeRadians;
    return 1;
}

int32_t omma_craft_orbit(const OmmaSim* sim, int32_t index, double* xyz,
                         int32_t count) {
    const Spacecraft* craft = craftAt(sim, index);
    if (craft == nullptr || !craft->isAlive()) {
        return 0;
    }
    const Vec3 origin = sim->world.system()
                            .bodies()[craft->centralBodyIndex]
                            ->sample(sim->world.now())
                            .position;
    return sampleEllipse(sim->world.elementsOf(*craft), origin, xyz, count);
}

}  // extern "C"
