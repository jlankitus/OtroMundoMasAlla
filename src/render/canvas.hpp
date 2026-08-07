// ─────────────────────────────────────────────────────────────────────────────
// Canvas — a hybrid pixel-and-character display for a terminal.
//
// THE HALF-BLOCK TRICK
// A terminal cell is roughly twice as tall as it is wide, which normally makes
// pixel art impossible. But the upper-half-block glyph U+2580 fills the top of
// its cell with the foreground colour and leaves the bottom showing the
// background colour:
//
//     ▀   foreground = the top pixel
//         background = the bottom pixel
//
// One cell becomes TWO SQUARE PIXELS. A 200x50 terminal is a 200x100
// framebuffer, and with 24-bit colour escapes it is a true-colour one. That is
// enough for shaded spheres, gradients and glow, with no dependency on
// anything and no loss of the ability to run over ssh or inside CI.
//
// TWO LAYERS, BECAUSE TEXT STILL HAS TO BE CRISP
// Drawing letters out of 2-pixel-tall blocks would need a bitmap font and
// would look terrible. So a Canvas holds a pixel layer AND a character layer,
// and a cell with a character in it wins. Pixels for the scene, glyphs for the
// HUD.
//
// ONE BUFFER, THREE PRESENTATIONS
// The same pixel data is emitted as true colour, as 16-colour ANSI, or as an
// ASCII density ramp, chosen at present() time. That last one matters more than
// it sounds: it keeps `--snapshot --no-colour` producing meaningful, diffable
// text instead of a blank rectangle, which is what makes the renderer testable
// at all.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace omma::render {

/// A 24-bit colour. Deliberately not a Vec3: these are bytes headed for an
/// escape sequence, and treating them as floats invites rounding drift through
/// every blend.
struct Rgb {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};

    friend constexpr bool operator==(const Rgb&, const Rgb&) noexcept = default;

    /// Perceptual brightness in [0, 1], using the Rec. 601 weights. Green
    /// dominates because the eye is most sensitive to it — a flat average makes
    /// blues look far brighter than they are.
    [[nodiscard]] constexpr double luminance() const noexcept {
        return (0.299 * static_cast<double>(r)
              + 0.587 * static_cast<double>(g)
              + 0.114 * static_cast<double>(b)) / 255.0;
    }

    /// Scale brightness, saturating rather than wrapping. Used for orbit trails
    /// (a dimmed body colour) and glow falloff.
    [[nodiscard]] constexpr Rgb scaled(double factor) const noexcept {
        const auto clamp = [](double v) -> std::uint8_t {
            if (v <= 0.0) return 0;
            if (v >= 255.0) return 255;
            return static_cast<std::uint8_t>(v + 0.5);
        };
        return Rgb{clamp(static_cast<double>(r) * factor),
                   clamp(static_cast<double>(g) * factor),
                   clamp(static_cast<double>(b) * factor)};
    }

    /// Additive blend, saturating. Overlapping glows accumulate.
    [[nodiscard]] constexpr Rgb plus(const Rgb& other) const noexcept {
        const auto add = [](std::uint8_t a, std::uint8_t b2) -> std::uint8_t {
            const int sum = static_cast<int>(a) + static_cast<int>(b2);
            return static_cast<std::uint8_t>(sum > 255 ? 255 : sum);
        };
        return Rgb{add(r, other.r), add(g, other.g), add(b, other.b)};
    }
};

inline constexpr Rgb kBlack{0, 0, 0};

/// The sixteen ANSI colours, for the character layer and for degraded output.
enum class Ink : std::uint8_t {
    Default = 0,
    Black, Red, Green, Yellow, Blue, Magenta, Cyan, White,
    BrightBlack, BrightRed, BrightGreen, BrightYellow,
    BrightBlue, BrightMagenta, BrightCyan, BrightWhite,
};

/// How much colour the terminal can be trusted with.
enum class ColourDepth : std::uint8_t {
    /// No escapes at all. Pixels become an ASCII density ramp. This is the
    /// mode that keeps snapshots diffable.
    Ascii,
    /// The sixteen colours every terminal has had since the 1980s.
    Ansi16,
    /// 24-bit. Windows Terminal, iTerm2, and essentially everything modern.
    TrueColour,
};

/// Whether the terminal can be trusted with a multi-byte glyph.
///
/// This is a SEPARATE capability from colour, and conflating them was a bug.
/// A Windows console honours 24-bit colour escapes perfectly well while still
/// decoding output as code page 437, in which case the three bytes of U+2580
/// arrive as three Latin-1 characters and the display is destroyed.
enum class BlockStyle : std::uint8_t {
    /// U+2580, two square pixels per cell. Full vertical resolution, needs the
    /// console to be in UTF-8.
    HalfBlocks,
    /// A space with a background colour: one pixel per cell, so the two pixel
    /// rows are averaged. Half the vertical resolution and works on literally
    /// any terminal, because a space is a space everywhere.
    FullCells,
};

class Canvas {
public:
    /// Dimensions are in CHARACTER CELLS. The pixel buffer is twice as tall.
    Canvas(int cellWidth, int cellHeight);

    void resize(int cellWidth, int cellHeight);

    /// Reset both layers: characters to blank, pixels to black.
    void clear() noexcept;

    // ── geometry ───────────────────────────────────────────────────────────
    [[nodiscard]] int width() const noexcept { return cellWidth_; }
    [[nodiscard]] int height() const noexcept { return cellHeight_; }
    [[nodiscard]] int pixelWidth() const noexcept { return cellWidth_; }
    [[nodiscard]] int pixelHeight() const noexcept { return cellHeight_ * 2; }

    [[nodiscard]] bool contains(int cx, int cy) const noexcept {
        return cx >= 0 && cy >= 0 && cx < cellWidth_ && cy < cellHeight_;
    }
    [[nodiscard]] bool containsPixel(int px, int py) const noexcept {
        return px >= 0 && py >= 0 && px < pixelWidth() && py < pixelHeight();
    }

    // ── character layer ────────────────────────────────────────────────────
    // Out-of-bounds writes are silently ignored throughout. Deliberate: a
    // renderer whose every call must be guarded by the caller is a renderer
    // full of duplicated guards, and clipping is what a viewport is for.

    void put(int cx, int cy, char glyph, Ink ink = Ink::Default) noexcept;
    void text(int cx, int cy, std::string_view s, Ink ink = Ink::Default) noexcept;

    /// Blank a rectangle of cells AND the pixels underneath them. Used to
    /// clear a strip before drawing HUD text over a busy scene.
    void fill(int cx, int cy, int cellsWide, int cellsHigh) noexcept;

    [[nodiscard]] char glyphAt(int cx, int cy) const noexcept;

    /// Is there room to draw a label here? True when the cells hold no
    /// character AND the pixels beneath are dark enough that text would still
    /// be readable over them.
    [[nodiscard]] bool spanIsClear(int cx, int cy, int length) const noexcept;

    // ── pixel layer ────────────────────────────────────────────────────────

    void setPixel(int px, int py, Rgb colour) noexcept;

    /// Additive write. Glow, overlapping trails, anything that should
    /// accumulate rather than replace.
    void addPixel(int px, int py, Rgb colour) noexcept;

    [[nodiscard]] Rgb pixelAt(int px, int py) const noexcept;

    /// Bresenham line. Orbits drawn as connected segments instead of scattered
    /// dots read as curves rather than as dashes, and they stay continuous when
    /// zoomed out far enough that adjacent samples are many pixels apart.
    void line(int x0, int y0, int x1, int y1, Rgb colour) noexcept;

    /// Filled circle, midpoint algorithm. Radius 0 still lights one pixel, so
    /// a body never disappears entirely just because it is small on screen.
    void disc(int cx, int cy, int radius, Rgb colour) noexcept;

    /// Radial falloff around a point, added rather than set. The Sun's corona.
    void glow(int cx, int cy, int radius, Rgb colour) noexcept;

    // ── output ─────────────────────────────────────────────────────────────

    /// Compose the frame into \p out for a single write.
    ///
    /// \param depth       how much colour to emit.
    /// \param blocks      whether to use the half-block glyph. See BlockStyle.
    /// \param homeCursor  emit cursor-home and per-line erase. On for the live
    ///                    loop, off for a one-shot snapshot that should be
    ///                    plain text.
    void present(std::string& out, ColourDepth depth,
                 BlockStyle blocks = BlockStyle::HalfBlocks,
                 bool homeCursor = true) const;

private:
    struct Cell {
        char glyph{' '};
        Ink  ink{Ink::Default};
    };

    [[nodiscard]] std::size_t cellIndex(int cx, int cy) const noexcept {
        return static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellWidth_)
             + static_cast<std::size_t>(cx);
    }
    [[nodiscard]] std::size_t pixelIndex(int px, int py) const noexcept {
        return static_cast<std::size_t>(py) * static_cast<std::size_t>(cellWidth_)
             + static_cast<std::size_t>(px);
    }

    int cellWidth_;
    int cellHeight_;
    std::vector<Cell> cells_;
    std::vector<Rgb>  pixels_;
};

}  // namespace omma::render
