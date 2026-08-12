// Canvas — a hybrid pixel-and-character display for a terminal.
//
// The upper-half-block glyph U+2580 takes an independent foreground (top
// pixel) and background (bottom pixel): two square pixels per character cell,
// so a 200x50 terminal is a 200x100 true-colour framebuffer. A separate
// character layer keeps HUD text crisp — a cell with a character wins over
// its pixels. present() emits true colour, 16-colour ANSI, or an ASCII
// density ramp; the ramp keeps --no-colour snapshots diffable. docs/DESIGN.md §7.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace omma::render {

/// A 24-bit colour: bytes headed for an escape sequence, deliberately not a
/// float Vec3 (rounding drift through every blend).
struct Rgb {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};

    friend constexpr bool operator==(const Rgb&, const Rgb&) noexcept = default;

    /// Perceptual brightness in [0, 1], Rec. 601 weights — green dominates.
    [[nodiscard]] constexpr double luminance() const noexcept {
        return (0.299 * static_cast<double>(r)
              + 0.587 * static_cast<double>(g)
              + 0.114 * static_cast<double>(b)) / 255.0;
    }

    /// Scale brightness, saturating rather than wrapping.
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

/// Named colours for the character layer — handles for Rgb values, not ANSI
/// codes: the sixteen-colour palette has no legible dim grey for HUD text on
/// a black scene, and what it has depends on the user's theme.
enum class Ink : std::uint8_t {
    Default = 0,
    Black, Red, Green, Yellow, Blue, Magenta, Cyan, White,
    BrightBlack, BrightRed, BrightGreen, BrightYellow,
    BrightBlue, BrightMagenta, BrightCyan, BrightWhite,
};

/// The Rgb an Ink stands for.
[[nodiscard]] Rgb inkToRgb(Ink ink) noexcept;

/// How much colour the terminal can be trusted with.
enum class ColourDepth : std::uint8_t {
    /// No escapes; pixels become an ASCII density ramp (diffable snapshots).
    Ascii,
    /// The sixteen colours every terminal has had since the 1980s.
    Ansi16,
    /// 24-bit. Windows Terminal, iTerm2, and essentially everything modern.
    TrueColour,
};

/// Whether the terminal can be trusted with a multi-byte glyph. A SEPARATE
/// capability from colour: a Windows console can honour 24-bit escapes while
/// still decoding output as CP437, which mangles the three bytes of U+2580.
enum class BlockStyle : std::uint8_t {
    /// U+2580, two square pixels per cell. Needs a UTF-8 console.
    HalfBlocks,
    /// A space with a background colour: one pixel per cell (the two pixel
    /// rows are averaged). Half the vertical resolution; works on any terminal.
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
    // Out-of-bounds writes are silently ignored throughout: clipping is the
    // renderer's job, not the caller's.

    void put(int cx, int cy, char glyph, Rgb fg) noexcept;
    void text(int cx, int cy, std::string_view s, Rgb fg) noexcept;

    void put(int cx, int cy, char glyph, Ink ink = Ink::Default) noexcept {
        put(cx, cy, glyph, inkToRgb(ink));
    }
    void text(int cx, int cy, std::string_view s, Ink ink = Ink::Default) noexcept {
        text(cx, cy, s, inkToRgb(ink));
    }

    /// Blank a rectangle of cells AND the pixels underneath them.
    void fill(int cx, int cy, int cellsWide, int cellsHigh) noexcept;

    [[nodiscard]] char glyphAt(int cx, int cy) const noexcept;

    /// Room to draw a label here? True when the cells hold no character AND
    /// the pixels beneath are dark enough for text to read over.
    [[nodiscard]] bool spanIsClear(int cx, int cy, int length) const noexcept;

    // ── pixel layer ────────────────────────────────────────────────────────

    void setPixel(int px, int py, Rgb colour) noexcept;

    /// Additive write: glow and overlapping trails accumulate.
    void addPixel(int px, int py, Rgb colour) noexcept;

    [[nodiscard]] Rgb pixelAt(int px, int py) const noexcept;

    /// Bresenham line, clipped to the canvas (Cohen–Sutherland): clipping is
    /// the renderer's job, so callers never pre-cull.
    void line(int x0, int y0, int x1, int y1, Rgb colour) noexcept;

    /// Filled circle, plain dx²+dy² ≤ r². Radius 0 still lights one pixel, so
    /// a body never disappears just because it is small on screen.
    void disc(int cx, int cy, int radius, Rgb colour) noexcept;

    /// A disc shaded as a lit sphere: Lambert's law against the analytic
    /// normal, nz = sqrt(1 - (dx^2 + dy^2) / r^2); the day/night terminator is
    /// where the dot product crosses zero. Brightness is quantised to 24
    /// levels to bound escape-sequence bandwidth.
    ///
    /// \param light    direction TOWARD the light, in screen space (x right,
    ///                 y down, z toward the viewer). Need not be normalised.
    /// \param ambient  floor brightness on the night side, 0..1; pure black
    ///                 reads as a bite out of the starfield.
    void shadedDisc(int cx, int cy, int radius, Rgb colour,
                    double lightX, double lightY, double lightZ,
                    double ambient = 0.10) noexcept;

    /// Radial falloff around a point, added rather than set. The Sun's corona.
    void glow(int cx, int cy, int radius, Rgb colour) noexcept;

    // ── output ─────────────────────────────────────────────────────────────

    /// Compose the frame into \p out for a single write.
    ///
    /// \param depth       how much colour to emit.
    /// \param blocks      whether to use the half-block glyph. See BlockStyle.
    /// \param homeCursor  emit cursor-home and per-line erase: on for the live
    ///                    loop, off for a plain-text snapshot.
    void present(std::string& out, ColourDepth depth,
                 BlockStyle blocks = BlockStyle::HalfBlocks,
                 bool homeCursor = true) const;

private:
    struct Cell {
        char glyph{' '};
        Rgb  fg{204, 204, 204};
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
