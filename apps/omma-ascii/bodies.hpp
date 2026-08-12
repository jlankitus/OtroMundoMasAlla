// ─────────────────────────────────────────────────────────────────────────────
// How each body looks. Pure presentation, kept as a table so adding a body is a
// one-line data change rather than a code change.
//
// THE DESIGN PROBLEM, AND WHY IT WAS NOT A COLOUR-PICKING PROBLEM
//
// The first version drew every orbit in a dimmed copy of its body's colour. The
// result was muddy and hard to read, and no amount of adjusting individual
// values fixed it, because the fault was structural: eleven lines all competing
// for attention at similar brightness. Mercury's grey, Venus' cream, Jupiter's
// tan and Saturn's gold are genuinely similar hues, so dimming them produced
// four indistinguishable browns.
//
// Star charts solved this a long time ago. Draw the reference geometry in ONE
// quiet colour, and give the thing under discussion its own bright one. So:
//
//   * every orbit is the same cool slate blue, dim, reading as scaffolding
//   * the FOCUSED body's orbit is drawn in its own colour, bright
//   * bodies stay fully saturated, and are the only saturated thing on screen
//
// That also makes `tab` meaningful: changing focus visibly changes what the
// picture is about. A palette change that adds information beats one that only
// adds colour.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "physics/solar_system.hpp"
#include "render/canvas.hpp"

#include <array>
#include <cstddef>

namespace omma::app {

/// Deep space. Not pure black: a hint of blue reads as sky rather than as a
/// hole, and it gives the dimmest orbit line something to sit against.
inline constexpr render::Rgb kSpace{4, 5, 10};

/// Every orbit except the focused one. Quiet enough to read as reference
/// geometry, bright enough to follow.
inline constexpr render::Rgb kOrbitReference{46, 58, 84};

/// How much of its own colour the focused body's orbit gets.
inline constexpr double kFocusedOrbitScale = 0.72;

struct BodyStyle {
    render::Rgb colour;     ///< the body itself, fully saturated
    double      glowScale;  ///< corona radius as a multiple of the disc; 0 for none
    char        glyph;      ///< fallback for the character layer
};

// Hue-separated on purpose. Mercury pulls toward lavender and Venus toward warm
// cream so the two innermost planets do not read as the same grey; Uranus is
// pushed to aqua and Neptune to a deep azure so they are told apart at a glance.
// Roughly what the eye would see, biased for distinguishability where reality is
// ambiguous — the job here is identification, not photometry.
inline constexpr std::array<BodyStyle, static_cast<std::size_t>(BodyId::Count)> kBodyStyles{{
    /* Sun     */ {{255, 232, 158}, 5.0, '@'},
    /* Mercury */ {{182, 176, 196}, 0.0, 'm'},
    /* Venus   */ {{246, 222, 168}, 0.0, 'V'},
    /* Earth   */ {{ 92, 172, 240}, 0.0, 'E'},
    /* Moon    */ {{216, 216, 208}, 0.0, '.'},
    /* Mars    */ {{234,  96,  64}, 0.0, 'M'},
    /* Jupiter */ {{226, 168,  98}, 0.0, 'J'},
    /* Saturn  */ {{238, 214, 140}, 0.0, 'S'},
    /* Uranus  */ {{126, 232, 226}, 0.0, 'U'},
    /* Neptune */ {{ 84, 118, 246}, 0.0, 'N'},
    /* Pluto   */ {{198, 168, 176}, 0.0, 'p'},
}};

[[nodiscard]] inline const BodyStyle& styleFor(std::size_t index) noexcept {
    return kBodyStyles[index < kBodyStyles.size() ? index : 0];
}

/// Colour for a body's orbit line, given whether it is the focused one.
[[nodiscard]] inline render::Rgb orbitColourFor(std::size_t index, bool focused) noexcept {
    return focused ? styleFor(index).colour.scaled(kFocusedOrbitScale) : kOrbitReference;
}

}  // namespace omma::app
