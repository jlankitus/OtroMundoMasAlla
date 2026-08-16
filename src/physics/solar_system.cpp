#include "physics/solar_system.hpp"

#include <array>

namespace omma {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Gravitational parameters, m^3/s^2. GM, not G times M: GM is measured
// directly from orbits to eleven or twelve digits, while G alone is known
// only to ~22 ppm. Planetary values are SYSTEM GMs (planet plus its moons)
// except Earth, listed separately from the Moon because we model the Moon.
// Source: JPL DE440 / IAU 2015 nominal values.
// ─────────────────────────────────────────────────────────────────────────────
constexpr double kGmSun     = 1.32712440041e20;
constexpr double kGmMercury = 2.20317800000e13;
constexpr double kGmVenus   = 3.24858592000e14;
constexpr double kGmEarth   = 3.98600435507e14;
constexpr double kGmMoon    = 4.90280001184e12;
constexpr double kGmMars    = 4.28283758157e13;
constexpr double kGmJupiter = 1.26712764100e17;
constexpr double kGmSaturn  = 3.79405852000e16;
constexpr double kGmUranus  = 5.79455600000e15;
constexpr double kGmNeptune = 6.83652710058e15;
constexpr double kGmPluto   = 8.69613817000e11;

// ── Mean radii, metres (IAU 2015) ────────────────────────────────────────────
constexpr double kRadiusSun     = 6.9570e8;
constexpr double kRadiusMercury = 2.4397e6;
constexpr double kRadiusVenus   = 6.0518e6;
constexpr double kRadiusEarth   = 6.3710e6;
constexpr double kRadiusMoon    = 1.7374e6;
constexpr double kRadiusMars    = 3.3895e6;
constexpr double kRadiusJupiter = 6.9911e7;
constexpr double kRadiusSaturn  = 5.8232e7;
constexpr double kRadiusUranus  = 2.5362e7;
constexpr double kRadiusNeptune = 2.4622e7;
constexpr double kRadiusPluto   = 1.1883e6;

// ─────────────────────────────────────────────────────────────────────────────
// Keplerian elements and rates, 1800 AD – 2050 AD. Transcribed verbatim from
// JPL's "Approximate Positions of the Major Planets", in the table's own
// units so the block can be diffed against the published page:
//     https://ssd.jpl.nasa.gov/planets/approx_pos.html
// Columns: a (au), e, I (deg), L (deg), longitude of periapsis (deg),
// node (deg); each value followed by its rate per Julian century.
// The "Earth" row is really the EARTH-MOON BARYCENTER (~4,670 km from
// Earth's centre) — invisible at solar-system scale, matters for a precision
// lunar mission.
// ─────────────────────────────────────────────────────────────────────────────

constexpr KeplerianRow kMercury{
    0.38709927,  0.00000037,
    0.20563593,  0.00001906,
    7.00497902, -0.00594749,
  252.25032350, 149472.67411175,
   77.45779628,  0.16047689,
   48.33076593, -0.12534081};

constexpr KeplerianRow kVenus{
    0.72333566,  0.00000390,
    0.00677672, -0.00004107,
    3.39467605, -0.00078890,
  181.97909950, 58517.81538729,
  131.60246718,  0.00268329,
   76.67984255, -0.27769418};

constexpr KeplerianRow kEarth{
    1.00000261,  0.00000562,
    0.01671123, -0.00004392,
   -0.00001531, -0.01294668,
  100.46457166, 35999.37244981,
  102.93768193,  0.32327364,
    0.0,         0.0};

constexpr KeplerianRow kMars{
    1.52371034,  0.00001847,
    0.09339410,  0.00007882,
    1.84969142, -0.00813131,
   -4.55343205, 19140.30268499,
  -23.94362959,  0.44441088,
   49.55953891, -0.29257343};

constexpr KeplerianRow kJupiter{
    5.20288700, -0.00011607,
    0.04838624, -0.00013253,
    1.30439695, -0.00183714,
   34.39644051, 3034.74612775,
   14.72847983,  0.21252668,
  100.47390909,  0.20469106};

constexpr KeplerianRow kSaturn{
    9.53667594, -0.00125060,
    0.05386179, -0.00050991,
    2.48599187,  0.00193609,
   49.95424423, 1222.49362201,
   92.59887831, -0.41897216,
  113.66242448, -0.28867794};

constexpr KeplerianRow kUranus{
   19.18916464, -0.00196176,
    0.04725744, -0.00004397,
    0.77263783, -0.00242939,
  313.23810451, 428.48202785,
  170.95427630,  0.40805281,
   74.01692503,  0.04240589};

constexpr KeplerianRow kNeptune{
   30.06992276,  0.00026291,
    0.00859048,  0.00005105,
    1.77004347,  0.00035372,
  -55.12002969, 218.45945325,
   44.96476227, -0.32241464,
  131.78422574, -0.00508664};

constexpr KeplerianRow kPluto{
   39.48211675, -0.00031596,
    0.24882730,  0.00005170,
   17.14001206,  0.00004818,
  238.92903833, 145.20780515,
  224.06891629, -0.04062942,
  110.30393684, -0.01183482};

// ─────────────────────────────────────────────────────────────────────────────
// The Moon — mean orbit around Earth, assembled from the standard mean lunar
// elements (not from the JPL planetary table), in the same row format.
//
// Accuracy warning: the Sun perturbs the Moon far beyond what a fixed ellipse
// can express (evection, variation, annual equation) — expect errors of a few
// thousand km; fine to watch, not to land with. What IS captured, being large
// and secular: nodal regression (one turn per 18.6 y) and apsidal precession
// (8.85 y). a is chosen consistent with the sidereal month (27.321661 d)
// given GM_earth + GM_moon, not the mean centre-to-centre distance; the two
// differ by ~1,350 km and mixing them up puts the period visibly wrong.
// ─────────────────────────────────────────────────────────────────────────────
constexpr double kMoonSemiMajorAxisAu = 3.84748e8 / 149'597'870'700.0;

constexpr KeplerianRow kMoon{
    kMoonSemiMajorAxisAu, 0.0,
    0.0549006,            0.0,
    5.1454,               0.0,
  218.3164477,       481267.88123421,   // sidereal month
   83.3532465,         4069.0137287,    // perigee: one turn per 8.85 years
  125.0445479,        -1934.1362891};   // node: one turn backwards per 18.6 years

struct BodySpec {
    BodyId              id;
    const char*         name;
    double              gm;
    double              radius;
    const KeplerianRow* row;      ///< nullptr for a fixed body
    BodyId              parent;   ///< ignored when row is nullptr
    /// J2 zonal harmonic and the equatorial radius it is defined against.
    /// Zero means treated as spherical; only bodies whose oblateness matters
    /// to an orbiter carry values (IERS/NASA planetary fact sheet figures).
    double              j2{0.0};
    double              equatorialRadius{0.0};
    /// Sidereal rotation period, seconds; zero means no rotation model.
    double              siderealDay{0.0};
    /// Orbits here decay by drag (Earth only; the density model is
    /// Earth-calibrated, so Mars stays false despite having air).
    bool                atmosphere{false};
};

// Order matters: a parent must appear before its children so the pointer is
// already valid when the child is constructed (the Moon after Earth, today).
constexpr std::array<BodySpec, static_cast<std::size_t>(BodyId::Count)> kBodySpecs{{
    {BodyId::Sun,     "Sun",     kGmSun,     kRadiusSun,     nullptr,   BodyId::Sun},
    {BodyId::Mercury, "Mercury", kGmMercury, kRadiusMercury, &kMercury, BodyId::Sun},
    {BodyId::Venus,   "Venus",   kGmVenus,   kRadiusVenus,   &kVenus,   BodyId::Sun},
    {BodyId::Earth,   "Earth",   kGmEarth,   kRadiusEarth,   &kEarth,   BodyId::Sun,
                      1.08262668e-3, 6'378'137.0, 86'164.0905, true},
    {BodyId::Moon,    "Moon",    kGmMoon,    kRadiusMoon,    &kMoon,    BodyId::Earth},
    {BodyId::Mars,    "Mars",    kGmMars,    kRadiusMars,    &kMars,    BodyId::Sun,
                      1.95545e-3, 3'396'190.0, 88'642.663},
    {BodyId::Jupiter, "Jupiter", kGmJupiter, kRadiusJupiter, &kJupiter, BodyId::Sun},
    {BodyId::Saturn,  "Saturn",  kGmSaturn,  kRadiusSaturn,  &kSaturn,  BodyId::Sun},
    {BodyId::Uranus,  "Uranus",  kGmUranus,  kRadiusUranus,  &kUranus,  BodyId::Sun},
    {BodyId::Neptune, "Neptune", kGmNeptune, kRadiusNeptune, &kNeptune, BodyId::Sun},
    {BodyId::Pluto,   "Pluto",   kGmPluto,   kRadiusPluto,   &kPluto,   BodyId::Sun},
}};

}  // namespace

std::string_view bodyName(BodyId id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kBodySpecs.size()) {
        return "unknown";
    }
    return kBodySpecs[index].name;
}

SolarSystem SolarSystem::standard() {
    SolarSystem system;
    system.bodies_.reserve(kBodySpecs.size());

    for (const BodySpec& spec : kBodySpecs) {
        if (spec.row == nullptr) {
            system.bodies_.push_back(
                std::make_unique<FixedEphemeris>(spec.name, spec.gm, spec.radius));
            continue;
        }

        const auto parentIndex = static_cast<std::size_t>(spec.parent);
        const IEphemeris* parent = system.bodies_[parentIndex].get();
        auto body = std::make_unique<KeplerEphemeris>(
            spec.name, spec.gm, spec.radius, *spec.row,
            parent, parent->gravitationalParameter());
        if (spec.j2 > 0.0) {
            body->setOblateness(spec.j2, spec.equatorialRadius);
        }
        if (spec.siderealDay > 0.0) {
            body->setSiderealRotationPeriod(spec.siderealDay);
        }
        body->setHasAtmosphere(spec.atmosphere);
        system.bodies_.push_back(std::move(body));
    }

    return system;
}

const IEphemeris& SolarSystem::operator[](BodyId id) const noexcept {
    return *bodies_[static_cast<std::size_t>(id)];
}

const IEphemeris* SolarSystem::findByName(std::string_view name) const noexcept {
    for (const auto& body : bodies_) {
        if (body->name() == name) {
            return body.get();
        }
    }
    return nullptr;
}

}  // namespace omma
