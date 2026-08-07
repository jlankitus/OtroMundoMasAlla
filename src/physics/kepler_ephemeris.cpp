#include "physics/kepler_ephemeris.hpp"

#include "core/units.hpp"

#include <utility>

namespace omma {
namespace {

/// Julian centuries elapsed since J2000. The unit JPL's rates are expressed in.
[[nodiscard]] double julianCenturiesSinceJ2000(Epoch t) noexcept {
    return t.daysSinceJ2000() / 36525.0;
}

}  // namespace

OrbitalElements elementsFromRow(const KeplerianRow& row, Epoch t) noexcept {
    const double T = julianCenturiesSinceJ2000(t);

    // Linear secular drift. This is a first-order model of what are really
    // long-period oscillations, which is why the published validity window is
    // 1800-2050 rather than "forever" — extrapolate far outside it and the
    // elements wander somewhere unphysical.
    const double aAu        = row.semiMajorAxisAu          + row.semiMajorAxisAuPerCy * T;
    const double e          = row.eccentricity             + row.eccentricityPerCy * T;
    const double iDeg       = row.inclinationDeg           + row.inclinationDegPerCy * T;
    const double lDeg       = row.meanLongitudeDeg         + row.meanLongitudeDegPerCy * T;
    const double varpiDeg   = row.longitudeOfPeriapsisDeg  + row.longitudeOfPeriapsisDegPerCy * T;
    const double raanDeg    = row.ascendingNodeDeg         + row.ascendingNodeDegPerCy * T;

    OrbitalElements out{};
    out.semiMajorAxis            = aAu * constants::kAu;
    out.eccentricity             = e;
    out.inclination              = toRadians(iDeg);
    out.longitudeOfAscendingNode = wrapToTwoPi(toRadians(raanDeg));
    out.argumentOfPeriapsis      = wrapToTwoPi(toRadians(varpiDeg - raanDeg));

    // The mean anomaly is already advanced to t, so the returned elements have
    // epoch == t. Anything downstream that propagates further will use
    // n = sqrt(GM/a^3), which is the physically consistent rate; it differs
    // from the table's own mean-longitude rate by a few parts in 100000.
    out.meanAnomalyAtEpoch = wrapToTwoPi(toRadians(lDeg - varpiDeg));
    out.epoch = t;
    return out;
}

KeplerEphemeris::KeplerEphemeris(std::string name,
                                 double gm,
                                 double meanRadius,
                                 KeplerianRow row,
                                 const IEphemeris* parent,
                                 double parentGm) noexcept
    : name_{std::move(name)},
      gm_{gm},
      meanRadius_{meanRadius},
      row_{row},
      parent_{parent},
      // Two bodies orbit their common barycenter, so the parameter governing
      // the relative orbit is the SUM of the two GMs, not the parent's alone.
      // For a planet around the Sun the difference is parts per million; for
      // the Moon around Earth it is 1.2%, worth about 4 hours of period.
      propagationGm_{parentGm + gm} {}

OrbitalElements KeplerEphemeris::elementsAt(Epoch t) const noexcept {
    return elementsFromRow(row_, t);
}

StateVector KeplerEphemeris::sampleRelativeToParent(Epoch t) const noexcept {
    return stateFromElements(elementsAt(t), propagationGm_, t);
}

StateVector KeplerEphemeris::sample(Epoch t) const noexcept {
    const StateVector relative = sampleRelativeToParent(t);
    return parent_ == nullptr ? relative : parent_->sample(t) + relative;
}

double KeplerEphemeris::orbitalPeriodSeconds(Epoch t) const noexcept {
    return orbitalPeriod(elementsAt(t), propagationGm_);
}

}  // namespace omma
