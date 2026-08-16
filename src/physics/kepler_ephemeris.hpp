// ─────────────────────────────────────────────────────────────────────────────
// KeplerEphemeris — a body on an analytic ellipse whose elements drift slowly.
//
// Elements are stored in the exact format JPL publishes them (au, degrees,
// rates per Julian century) so the table can be diffed against the published
// page character by character; conversion to SI happens once, on evaluation.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "physics/ephemeris.hpp"
#include "physics/orbital_elements.hpp"

#include <string>

namespace omma {

/// One row of JPL's "Approximate Positions of the Major Planets" table:
/// a value at J2000 plus a linear rate per Julian century, in published units
/// (au, au/Cy, degrees, degrees/Cy). The table parameterises orientation with
/// the *longitude* of periapsis and the *mean longitude*, related to ours by
///     omega = varpi - Omega,   M = L - varpi
/// — the conversion in elementsAt().
///
/// Source: https://ssd.jpl.nasa.gov/planets/approx_pos.html
/// Accuracy: a few arcminutes over 1800–2050; swappable for real DE440
/// kernels behind the same IEphemeris.
struct KeplerianRow {
    double semiMajorAxisAu{0.0};              double semiMajorAxisAuPerCy{0.0};
    double eccentricity{0.0};                 double eccentricityPerCy{0.0};
    double inclinationDeg{0.0};               double inclinationDegPerCy{0.0};
    double meanLongitudeDeg{0.0};             double meanLongitudeDegPerCy{0.0};
    double longitudeOfPeriapsisDeg{0.0};      double longitudeOfPeriapsisDegPerCy{0.0};
    double ascendingNodeDeg{0.0};             double ascendingNodeDegPerCy{0.0};
};

/// Evaluate a JPL row at time \p t and convert to SI orbital elements.
///
/// The returned elements have `epoch == t` and a mean anomaly already advanced
/// to that instant, so they describe the osculating ellipse *right now*.
[[nodiscard]] OrbitalElements elementsFromRow(const KeplerianRow& row, Epoch t) noexcept;

class KeplerEphemeris final : public IEphemeris {
public:
    /// \param parent    the body this one orbits, or nullptr for a root body.
    ///                  Non-owning; must outlive this object. SolarSystem owns
    ///                  both and guarantees the ordering.
    /// \param parentGm  GM of the parent. The propagation uses parentGm + gm,
    ///                  which is the correct two-body parameter — the pair
    ///                  orbits their common barycenter, not the parent's
    ///                  centre. For the Moon this is a 1.2% correction to the
    ///                  period and very much not negligible.
    KeplerEphemeris(std::string name,
                    double gm,
                    double meanRadius,
                    KeplerianRow row,
                    const IEphemeris* parent,
                    double parentGm) noexcept;

    /// Absolute state in the root frame: the parent's state plus our own
    /// offset from it. Recurses up the chain, which is at most Sun→planet→moon.
    [[nodiscard]] StateVector sample(Epoch t) const noexcept override;

    /// State relative to the parent body. This is the orbit proper.
    [[nodiscard]] StateVector sampleRelativeToParent(Epoch t) const noexcept;

    /// Osculating elements at \p t, in SI.
    [[nodiscard]] OrbitalElements elementsAt(Epoch t) const noexcept;

    [[nodiscard]] double gravitationalParameter() const noexcept override { return gm_; }
    [[nodiscard]] double meanRadius() const noexcept override { return meanRadius_; }
    [[nodiscard]] double j2() const noexcept override { return j2_; }
    [[nodiscard]] double equatorialRadius() const noexcept override {
        return equatorialRadius_ > 0.0 ? equatorialRadius_ : meanRadius_;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    /// Give the body an oblateness. Separate from the constructor because only
    /// a few bodies have measured J2 worth modelling.
    void setOblateness(double j2, double equatorialRadius) noexcept {
        j2_ = j2;
        equatorialRadius_ = equatorialRadius;
    }

    [[nodiscard]] double siderealRotationPeriod() const noexcept override {
        return siderealRotationPeriod_;
    }
    void setSiderealRotationPeriod(double seconds) noexcept {
        siderealRotationPeriod_ = seconds;
    }

    /// GM used to propagate this orbit: parent's GM plus our own.
    [[nodiscard]] double propagationGm() const noexcept { return propagationGm_; }
    [[nodiscard]] const IEphemeris* parent() const noexcept { return parent_; }

    /// Orbital period around the parent, seconds.
    [[nodiscard]] double orbitalPeriodSeconds(Epoch t) const noexcept;

private:
    std::string       name_;
    double            gm_;
    double            meanRadius_;
    double            j2_{0.0};
    double            equatorialRadius_{0.0};
    double            siderealRotationPeriod_{0.0};
    KeplerianRow      row_;
    const IEphemeris* parent_;
    double            propagationGm_;
};

}  // namespace omma
