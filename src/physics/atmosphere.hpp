// Atmospheric density for drag. A piecewise-exponential fit to the standard
// atmosphere (Vallado, Fundamentals of Astrodynamics, Table 8-4): within each
// altitude band, rho = rho0 * exp(-(h - h0) / H). Static model — no solar
// activity, no day/night bulge — which is the right first rung: real density
// varies by a factor of several with solar weather, so drag predictions are
// order-of-magnitude by nature and this model is honest about it.
#pragma once

namespace omma {

/// Density of Earth's atmosphere at \p altitudeMetres above the mean surface,
/// kg/m^3. Below the lowest band the last exponential continues downward
/// (re-entry: density grows fast and the orbit is over); far above, it decays
/// toward nothing.
[[nodiscard]] double earthAtmosphericDensity(double altitudeMetres) noexcept;

}  // namespace omma
