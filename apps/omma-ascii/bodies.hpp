// ─────────────────────────────────────────────────────────────────────────────
// How each body looks. Pure presentation data, kept in a table so adding a
// body is a one-line data change rather than a code change.
//
// Colours are roughly what the eye would see: Mars' iron oxide, Jupiter's
// ammonia bands, Neptune's methane blue. Not measured spectra — just close
// enough that you can identify a planet without reading the label.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "physics/solar_system.hpp"
#include "render/canvas.hpp"

#include <array>
#include <cstddef>

namespace omma::app {

struct BodyStyle {
    render::Rgb colour;      ///< the body itself
    double      trailScale;  ///< how bright its orbit line is, 0..1
    double      glowScale;   ///< corona radius as a multiple of the disc, 0 for none
    char        glyph;       ///< fallback for the character layer
};

// trailScale was first tuned against the ASCII density ramp, where 0.16 still
// mapped to a visible '.'. In true colour the same value is rgb(27,26,25) --
// indistinguishable from black. A brightness that reads correctly through one
// presentation can be invisible through another, which is a good reason to look
// at the actual output rather than trusting a number that "seemed fine".
inline constexpr std::array<BodyStyle, static_cast<std::size_t>(BodyId::Count)> kBodyStyles{{
    /* Sun     */ {{255, 236, 170}, 0.00, 5.0, '@'},
    /* Mercury */ {{168, 162, 155}, 0.42, 0.0, 'm'},
    /* Venus   */ {{234, 214, 172}, 0.42, 0.0, 'V'},
    /* Earth   */ {{ 96, 148, 224}, 0.60, 0.0, 'E'},
    /* Moon    */ {{198, 196, 188}, 0.45, 0.0, '.'},
    /* Mars    */ {{198,  94,  58}, 0.55, 0.0, 'M'},
    /* Jupiter */ {{212, 176, 132}, 0.45, 0.0, 'J'},
    /* Saturn  */ {{226, 205, 152}, 0.42, 0.0, 'S'},
    /* Uranus  */ {{146, 214, 222}, 0.42, 0.0, 'U'},
    /* Neptune */ {{ 84, 118, 214}, 0.50, 0.0, 'N'},
    /* Pluto   */ {{188, 174, 160}, 0.38, 0.0, 'p'},
}};

[[nodiscard]] inline const BodyStyle& styleFor(std::size_t index) noexcept {
    return kBodyStyles[index < kBodyStyles.size() ? index : 0];
}

}  // namespace omma::app
