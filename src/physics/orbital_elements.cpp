#include "physics/orbital_elements.hpp"

#include "core/units.hpp"

#include <cmath>
#include <limits>

namespace omma {
namespace {

constexpr double kTwoPi = constants::kTwoPi;
constexpr double kPi = constants::kPi;

/// Below this, an orbit is treated as circular and periapsis is undefined.
constexpr double kCircularTolerance = 1e-11;
/// Below this, an orbit is treated as equatorial and the node is undefined.
constexpr double kEquatorialTolerance = 1e-11;

constexpr int kNewtonMaxIterations = 60;
constexpr double kNewtonTolerance = 1e-14;

}  // namespace

double wrapToTwoPi(double radians) noexcept {
    const double wrapped = std::fmod(radians, kTwoPi);
    return wrapped < 0.0 ? wrapped + kTwoPi : wrapped;
}

double wrapToPi(double radians) noexcept {
    const double wrapped = wrapToTwoPi(radians + kPi);
    return wrapped - kPi;
}

double periapsisRadius(const OrbitalElements& e) noexcept {
    return e.semiMajorAxis * (1.0 - e.eccentricity);
}

double apoapsisRadius(const OrbitalElements& e) noexcept {
    if (e.eccentricity >= 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    return e.semiMajorAxis * (1.0 + e.eccentricity);
}

double meanMotion(const OrbitalElements& e, double gm) noexcept {
    const double a = std::abs(e.semiMajorAxis);
    return std::sqrt(gm / (a * a * a));
}

double orbitalPeriod(const OrbitalElements& e, double gm) noexcept {
    if (e.eccentricity >= 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    return kTwoPi / meanMotion(e, gm);
}

double meanAnomalyAt(const OrbitalElements& e, double gm, Epoch t) noexcept {
    const double dt = toSeconds(t - e.epoch);
    return wrapToTwoPi(e.meanAnomalyAtEpoch + meanMotion(e, gm) * dt);
}

// ─────────────────────────────────────────────────────────────────────────────

double solveKeplerEquation(double meanAnomaly, double eccentricity) noexcept {
    const double e = eccentricity;
    const double M = wrapToPi(meanAnomaly);   // symmetric range keeps Newton tame

    if (e < kCircularTolerance) {
        return M;                              // circular: E == M exactly
    }

    // Near e = 1 the curve flattens at periapsis and Newton from a nearby
    // guess can be thrown far; starting from pi keeps it on the correct side.
    double E = (e < 0.8) ? (M + e * std::sin(M))
                         : (M < 0.0 ? -kPi : kPi);

    for (int i = 0; i < kNewtonMaxIterations; ++i) {
        const double f = E - e * std::sin(E) - M;
        const double fPrime = 1.0 - e * std::cos(E);

        // fPrime >= 1 - e > 0 for e < 1; trips only on denormals or e >= 1.
        if (std::abs(fPrime) < 1e-300) {
            break;
        }
        const double delta = f / fPrime;
        E -= delta;
        if (std::abs(delta) < kNewtonTolerance) {
            return E;
        }
    }

    // Newton did not converge: bisect on [M - 1, M + 1]. |E - M| <= e < 1
    // puts the root inside the bracket, and the function is strictly
    // increasing for e < 1, so bisection cannot fail — merely slower.
    double low = M - 1.0;
    double high = M + 1.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (low + high);
        const double f = mid - e * std::sin(mid) - M;
        if (f > 0.0) {
            high = mid;
        } else {
            low = mid;
        }
    }
    return 0.5 * (low + high);
}

double trueAnomalyFromEccentric(double eccentricAnomaly, double eccentricity) noexcept {
    // atan2 half-angle form: keeps the quadrant, unlike acos, which needs a
    // sign fix-up that is easy to get wrong.
    const double e = eccentricity;
    const double halfE = 0.5 * eccentricAnomaly;
    return 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(halfE),
                            std::sqrt(1.0 - e) * std::cos(halfE));
}

double eccentricAnomalyFromTrue(double trueAnomaly, double eccentricity) noexcept {
    const double e = eccentricity;
    const double halfNu = 0.5 * trueAnomaly;
    return 2.0 * std::atan2(std::sqrt(1.0 - e) * std::sin(halfNu),
                            std::sqrt(1.0 + e) * std::cos(halfNu));
}

// ─────────────────────────────────────────────────────────────────────────────

PerifocalBasis perifocalBasis(const OrbitalElements& elements) noexcept {
    const double cosO = std::cos(elements.longitudeOfAscendingNode);
    const double sinO = std::sin(elements.longitudeOfAscendingNode);
    const double cosW = std::cos(elements.argumentOfPeriapsis);
    const double sinW = std::sin(elements.argumentOfPeriapsis);
    const double cosI = std::cos(elements.inclination);
    const double sinI = std::sin(elements.inclination);

    return PerifocalBasis{
        Vec3{cosO * cosW - sinO * sinW * cosI,
             sinO * cosW + cosO * sinW * cosI,
             sinW * sinI},
        Vec3{-cosO * sinW - sinO * cosW * cosI,
             -sinO * sinW + cosO * cosW * cosI,
             cosW * sinI}};
}

StateVector stateFromElements(const OrbitalElements& elements, double gm, Epoch t) noexcept {
    const double a = elements.semiMajorAxis;
    const double e = elements.eccentricity;

    const double M = meanAnomalyAt(elements, gm, t);
    const double E = solveKeplerEquation(M, e);

    const double cosE = std::cos(E);
    const double sinE = std::sin(E);
    const double sqrtOneMinusESq = std::sqrt(1.0 - e * e);

    // Position in the perifocal frame: x toward periapsis, y 90 degrees ahead
    // in the direction of motion, z along the angular momentum vector.
    const double xPerifocal = a * (cosE - e);
    const double yPerifocal = a * sqrtOneMinusESq * sinE;

    // dE/dt from differentiating Kepler's equation: n = (1 - e cos E) dE/dt.
    const double n = meanMotion(elements, gm);
    const double eDot = n / (1.0 - e * cosE);
    const double vxPerifocal = -a * sinE * eDot;
    const double vyPerifocal = a * sqrtOneMinusESq * cosE * eDot;

    const auto [p, q] = perifocalBasis(elements);
    return StateVector{p * xPerifocal + q * yPerifocal,
                       p * vxPerifocal + q * vyPerifocal};
}

OrbitalElements elementsFromState(const StateVector& state, double gm, Epoch t) noexcept {
    const Vec3& r = state.position;
    const Vec3& v = state.velocity;

    const double rMag = r.norm();
    const double vSq = v.normSquared();

    // Angular momentum defines the orbital plane.
    const Vec3 h = cross(r, v);
    const double hMag = h.norm();

    // Eccentricity vector: points at periapsis, magnitude is e.
    //
    //     e = [ (v^2 - GM/r) r  -  (r . v) v ] / GM
    //
    // The scalar (v^2 - GM/r) multiplies the POSITION and (r.v) multiplies the
    // VELOCITY, not the other way round. Transposed, it looks plausible on
    // eccentric orbits and is badly wrong on near-circular ones -- exactly the
    // orbits real satellites fly.
    const Vec3 eVec = (r * (vSq - gm / rMag) - v * dot(r, v)) / gm;
    const double e = eVec.norm();

    // Vis-viva rearranged: 1/a = 2/r - v^2/GM.
    const double inverseA = 2.0 / rMag - vSq / gm;
    const double a = 1.0 / inverseA;

    const double i = std::atan2(std::sqrt(h.x * h.x + h.y * h.y), h.z);

    // Node vector: z-hat cross h, pointing at the ascending node.
    const Vec3 nodeVec{-h.y, h.x, 0.0};
    const double nodeMag = nodeVec.norm();

    OrbitalElements out{};
    out.semiMajorAxis = a;
    out.eccentricity = e;
    out.inclination = i;
    out.epoch = t;

    const bool equatorial = nodeMag < kEquatorialTolerance * hMag;
    const bool circular = e < kCircularTolerance;

    // RAAN is undefined for an equatorial orbit; convention sets it to zero
    // and lets the argument of periapsis absorb the rotation.
    const double raan = equatorial ? 0.0 : wrapToTwoPi(std::atan2(nodeVec.y, nodeVec.x));
    out.longitudeOfAscendingNode = raan;

    // Argument of periapsis and true anomaly, chosen per degenerate case so
    // that every branch round-trips through stateFromElements.
    double argumentOfPeriapsis = 0.0;
    double trueAnomaly = 0.0;

    if (!equatorial && !circular) {
        argumentOfPeriapsis = wrapToTwoPi(std::atan2(dot(cross(nodeVec, eVec), h) / hMag,
                                                     dot(nodeVec, eVec)));
        trueAnomaly = wrapToTwoPi(std::atan2(dot(cross(eVec, r), h) / hMag, dot(eVec, r)));
    } else if (!equatorial && circular) {
        // No periapsis: measure the body's angle from the ascending node
        // instead (the "argument of latitude") and call it the anomaly.
        argumentOfPeriapsis = 0.0;
        trueAnomaly = wrapToTwoPi(std::atan2(dot(cross(nodeVec, r), h) / hMag, dot(nodeVec, r)));
    } else if (equatorial && !circular) {
        // No node: measure periapsis from the x-axis ("longitude of
        // periapsis"). Retrograde orbits need the sign flip.
        argumentOfPeriapsis = wrapToTwoPi(std::atan2(h.z >= 0.0 ? eVec.y : -eVec.y, eVec.x));
        trueAnomaly = wrapToTwoPi(std::atan2(dot(cross(eVec, r), h) / hMag, dot(eVec, r)));
    } else {
        // Neither node nor periapsis: the true longitude is all that is left.
        argumentOfPeriapsis = 0.0;
        trueAnomaly = wrapToTwoPi(std::atan2(h.z >= 0.0 ? r.y : -r.y, r.x));
    }

    out.argumentOfPeriapsis = argumentOfPeriapsis;

    if (e < 1.0) {
        const double E = eccentricAnomalyFromTrue(trueAnomaly, e);
        out.meanAnomalyAtEpoch = wrapToTwoPi(E - e * std::sin(E));
    } else {
        // Hyperbolic: mean anomaly via hyperbolic functions. Not yet
        // exercised; gets its own tests when escape trajectories arrive.
        const double H = 2.0 * std::atanh(std::sqrt((e - 1.0) / (e + 1.0))
                                          * std::tan(0.5 * trueAnomaly));
        out.meanAnomalyAtEpoch = e * std::sinh(H) - H;
    }

    return out;
}

double specificOrbitalEnergy(const StateVector& state, double gm) noexcept {
    return 0.5 * state.velocity.normSquared() - gm / state.position.norm();
}

Vec3 specificAngularMomentum(const StateVector& state) noexcept {
    return cross(state.position, state.velocity);
}

}  // namespace omma
