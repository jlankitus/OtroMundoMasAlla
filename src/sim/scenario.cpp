#include "sim/scenario.hpp"

#include "core/units.hpp"

#include <cstdio>
#include <iterator>

namespace omma {

std::optional<Scenario> scenarioFromName(std::string_view name) {
    if (name == "empty")         return Scenario::Empty;
    if (name == "leo")           return Scenario::Leo;
    if (name == "constellation") return Scenario::Constellation;
    if (name == "transfer")      return Scenario::Transfer;
    return std::nullopt;
}

LaunchRequest makeLaunchRequest(BodyId around, int index) {
    LaunchRequest request{};
    char name[24];
    std::snprintf(name, sizeof(name), "OMMA-%d", index);
    request.name = name;
    request.aroundBody = around;
    request.altitudeMetres = 400.0e3;
    request.inclinationRadians = toRadians(51.6);
    request.meanAnomalyRadians = toRadians(37.0 * static_cast<double>(index));
    request.longitudeOfAscendingNodeRadians =
        toRadians(23.0 * static_cast<double>(index));
    request.propellantKg = 80.0;
    request.maxThrustNewtons = 400.0;   // ~1.4 m/s^2: burns take seconds, not hours
    return request;
}

void applyScenario(World& world, Scenario scenario) {
    switch (scenario) {
        case Scenario::Empty:
            return;

        case Scenario::Leo: {
            // Three real mission profiles, chosen to look different from each
            // other at a glance rather than to be a tidy set.
            struct Orbit { const char* name; double altKm; double incDeg; double raanDeg;
                           double phaseDeg; };
            constexpr Orbit kOrbits[] = {
                {"ISS-LIKE",  400.0,  51.6,   0.0,   0.0},   // crewed inclination
                {"SUN-SYNC",  705.0,  98.2,  40.0, 120.0},   // earth observation
                {"EQUATOR",   550.0,   5.0,  90.0, 240.0},   // low-inclination comms
            };
            for (int i = 0; i < static_cast<int>(std::size(kOrbits)); ++i) {
                const Orbit& orbit = kOrbits[i];
                LaunchRequest request = makeLaunchRequest(BodyId::Earth, i + 1);
                request.name = orbit.name;
                request.altitudeMetres = orbit.altKm * 1000.0;
                request.inclinationRadians = toRadians(orbit.incDeg);
                request.longitudeOfAscendingNodeRadians = toRadians(orbit.raanDeg);
                request.meanAnomalyRadians = toRadians(orbit.phaseDeg);
                world.launch(request);
            }
            return;
        }

        case Scenario::Constellation: {
            // A Walker-style set: four planes, three satellites each, evenly
            // spread in right ascension and phased within each plane — the
            // layout GPS, Iridium and Starlink actually use.
            constexpr int kPlanes = 4;
            constexpr int kPerPlane = 3;
            int index = 0;
            for (int plane = 0; plane < kPlanes; ++plane) {
                for (int slot = 0; slot < kPerPlane; ++slot) {
                    LaunchRequest request = makeLaunchRequest(BodyId::Earth, ++index);
                    char name[24];
                    std::snprintf(name, sizeof(name), "W-%d%d", plane + 1, slot + 1);
                    request.name = name;
                    request.altitudeMetres = 780.0e3;
                    request.inclinationRadians = toRadians(86.4);
                    request.longitudeOfAscendingNodeRadians =
                        toRadians(180.0 * plane / kPlanes);
                    request.meanAnomalyRadians =
                        toRadians(360.0 * slot / kPerPlane
                                  + 20.0 * plane);   // inter-plane phasing
                    world.launch(request);
                }
            }
            return;
        }

        case Scenario::Transfer: {
            // One craft with a prograde burn already commanded: the predicted
            // ellipse visibly elongates while periapsis stays put, with
            // nothing to press.
            LaunchRequest request = makeLaunchRequest(BodyId::Earth, 1);
            request.name = "TRANSFER";
            request.altitudeMetres = 400.0e3;
            request.inclinationRadians = toRadians(28.5);
            request.propellantKg = 140.0;
            const auto id = world.launch(request);
            world.commandDeltaV(id, ThrustCommand::Frame::Prograde, 900.0);
            return;
        }
    }
}

}  // namespace omma
