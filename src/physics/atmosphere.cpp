#include "physics/atmosphere.hpp"

#include <cmath>
#include <cstddef>
#include <iterator>

namespace omma {
namespace {

/// One exponential band: base altitude (km), density there (kg/m^3), and the
/// scale height (km). Vallado Table 8-4, the rows an orbiter can reach.
struct Band {
    double baseKm;
    double density;
    double scaleKm;
};

constexpr Band kBands[] = {
    // The lower atmosphere exists because rockets launch through it. The
    // first version started at 100 km — fine for every satellite, and wrong
    // by 10x at sea level the moment the first ascent flew (the 100 km band's
    // thermosphere scale height, extrapolated down, gives 12.9 kg/m^3 at the
    // pad; the real number is 1.225).
    {0.0,   1.225,     7.249},
    {25.0,  3.899e-2,  6.349},
    {30.0,  1.774e-2,  6.682},
    {40.0,  3.972e-3,  7.554},
    {50.0,  1.057e-3,  8.382},
    {60.0,  3.206e-4,  7.714},
    {70.0,  8.770e-5,  6.549},
    {80.0,  1.905e-5,  5.799},
    {90.0,  3.396e-6,  5.382},
    {100.0, 5.297e-7,  5.877},
    {150.0, 2.070e-9,  22.523},
    {200.0, 2.789e-10, 37.105},
    {250.0, 7.248e-11, 45.546},
    {300.0, 2.418e-11, 53.628},
    {350.0, 9.518e-12, 53.298},
    {400.0, 3.725e-12, 58.515},
    {450.0, 1.585e-12, 60.828},
    {500.0, 6.967e-13, 63.822},
    {600.0, 1.454e-13, 71.835},
    {700.0, 3.614e-14, 88.667},
    {800.0, 1.170e-14, 124.64},
    {900.0, 5.245e-15, 181.05},
    {1000.0, 3.019e-15, 268.00},
};

}  // namespace

double earthAtmosphericDensity(double altitudeMetres) noexcept {
    const double km = altitudeMetres / 1000.0;

    // Find the band this altitude falls in; below the first, the first band's
    // exponential continues downward, and above the last, the last continues up.
    std::size_t band = 0;
    for (std::size_t i = 1; i < std::size(kBands); ++i) {
        if (km >= kBands[i].baseKm) {
            band = i;
        }
    }
    const Band& b = kBands[band];
    return b.density * std::exp(-(km - b.baseKm) / b.scaleKm);
}

}  // namespace omma
