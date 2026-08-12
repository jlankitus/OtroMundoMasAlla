#include "render/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace omma::render {
namespace {

/// Rgb for each Ink. Index matches the enum. Chosen for legibility on a
/// near-black scene, not to imitate a terminal palette — in particular
/// BrightBlack is a readable slate grey.
constexpr Rgb kInkColours[] = {
    /* Default       */ {204, 208, 214},
    /* Black         */ { 12,  14,  18},
    /* Red           */ {214,  90,  74},
    /* Green         */ { 96, 190, 126},
    /* Yellow        */ {214, 178,  96},
    /* Blue          */ { 90, 130, 214},
    /* Magenta       */ {186, 110, 190},
    /* Cyan          */ { 96, 186, 196},
    /* White         */ {188, 194, 204},
    /* BrightBlack   */ {132, 142, 158},
    /* BrightRed     */ {246, 122,  98},
    /* BrightGreen   */ {132, 226, 156},
    /* BrightYellow  */ {248, 214, 128},
    /* BrightBlue    */ {126, 168, 246},
    /* BrightMagenta */ {224, 148, 226},
    /* BrightCyan    */ {126, 226, 234},
    /* BrightWhite   */ {242, 246, 252},
};
static_assert(std::size(kInkColours) == 17, "one colour per Ink value");

/// Density ramp for the no-colour presentation, darkest first. Ordered by
/// perceived brightness at terminal sizes, not by ASCII order.
constexpr std::string_view kDensityRamp = " .:-=+*#%@";

/// A label needs the pixels under it to be dark enough to read over.
constexpr double kLabelLuminanceLimit = 0.12;

void appendUnsigned(std::string& out, unsigned value) {
    char buffer[4];
    int length = 0;
    if (value >= 100) buffer[length++] = static_cast<char>('0' + (value / 100) % 10);
    if (value >= 10)  buffer[length++] = static_cast<char>('0' + (value / 10) % 10);
    buffer[length++] = static_cast<char>('0' + value % 10);
    out.append(buffer, static_cast<std::size_t>(length));
}

/// "\033[38;2;R;G;Bm" or the background equivalent, built by hand: snprintf
/// is ~8x slower and this runs up to twice per cell, every cell, every frame.
void appendTrueColour(std::string& out, Rgb colour, bool background) {
    out += background ? "\033[48;2;" : "\033[38;2;";
    appendUnsigned(out, colour.r);
    out += ';';
    appendUnsigned(out, colour.g);
    out += ';';
    appendUnsigned(out, colour.b);
    out += 'm';
}

/// Nearest of the sixteen ANSI colours, for terminals without 24-bit support:
/// each channel quantised to one bit, plus a brightness bit. Crude, and
/// adequate for a sixteen-colour target the user's theme redefines anyway.
int nearestAnsi16(Rgb colour) {
    const int threshold = 96;
    const int bright = 168;
    const int bits = (colour.r > threshold ? 1 : 0)
                   | (colour.g > threshold ? 2 : 0)
                   | (colour.b > threshold ? 4 : 0);
    const bool isBright =
        std::max({colour.r, colour.g, colour.b}) > static_cast<std::uint8_t>(bright);
    return (isBright ? 90 : 30) + bits;
}

void appendAnsi16(std::string& out, Rgb colour, bool background) {
    const int code = nearestAnsi16(colour) + (background ? 10 : 0);
    out += "\033[";
    appendUnsigned(out, static_cast<unsigned>(code));
    out += 'm';
}

}  // namespace

Rgb inkToRgb(Ink ink) noexcept {
    const auto index = static_cast<std::size_t>(ink);
    return kInkColours[index < std::size(kInkColours) ? index : 0];
}

Canvas::Canvas(int cellWidth, int cellHeight)
    : cellWidth_{std::max(1, cellWidth)}, cellHeight_{std::max(1, cellHeight)} {
    cells_.assign(static_cast<std::size_t>(cellWidth_) * static_cast<std::size_t>(cellHeight_),
                  Cell{});
    pixels_.assign(static_cast<std::size_t>(cellWidth_)
                       * static_cast<std::size_t>(cellHeight_) * 2,
                   kBlack);
}

void Canvas::resize(int cellWidth, int cellHeight) {
    cellWidth_ = std::max(1, cellWidth);
    cellHeight_ = std::max(1, cellHeight);
    cells_.assign(static_cast<std::size_t>(cellWidth_) * static_cast<std::size_t>(cellHeight_),
                  Cell{});
    pixels_.assign(static_cast<std::size_t>(cellWidth_)
                       * static_cast<std::size_t>(cellHeight_) * 2,
                   kBlack);
}

void Canvas::clear() noexcept {
    std::fill(cells_.begin(), cells_.end(), Cell{});
    std::fill(pixels_.begin(), pixels_.end(), kBlack);
}

// ─── character layer ─────────────────────────────────────────────────────────

void Canvas::put(int cx, int cy, char glyph, Rgb fg) noexcept {
    if (!contains(cx, cy)) {
        return;
    }
    cells_[cellIndex(cx, cy)] = Cell{glyph, fg};
}

void Canvas::text(int cx, int cy, std::string_view s, Rgb fg) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        put(cx + static_cast<int>(i), cy, s[i], fg);
    }
}

void Canvas::fill(int cx, int cy, int cellsWide, int cellsHigh) noexcept {
    for (int row = cy; row < cy + cellsHigh; ++row) {
        for (int column = cx; column < cx + cellsWide; ++column) {
            if (!contains(column, row)) {
                continue;
            }
            cells_[cellIndex(column, row)] = Cell{};
            pixels_[pixelIndex(column, row * 2)] = kBlack;
            pixels_[pixelIndex(column, row * 2 + 1)] = kBlack;
        }
    }
}

char Canvas::glyphAt(int cx, int cy) const noexcept {
    return contains(cx, cy) ? cells_[cellIndex(cx, cy)].glyph : ' ';
}

bool Canvas::spanIsClear(int cx, int cy, int length) const noexcept {
    if (cy < 0 || cy >= cellHeight_) {
        return false;
    }
    for (int i = 0; i < length; ++i) {
        const int column = cx + i;
        if (column < 0 || column >= cellWidth_) {
            return false;      // would be clipped: treat as no room
        }
        if (cells_[cellIndex(column, cy)].glyph != ' ') {
            return false;
        }
        const double top = pixels_[pixelIndex(column, cy * 2)].luminance();
        const double bottom = pixels_[pixelIndex(column, cy * 2 + 1)].luminance();
        if (std::max(top, bottom) > kLabelLuminanceLimit) {
            return false;      // a body or a bright trail is already here
        }
    }
    return true;
}

// ─── pixel layer ─────────────────────────────────────────────────────────────

void Canvas::setPixel(int px, int py, Rgb colour) noexcept {
    if (!containsPixel(px, py)) {
        return;
    }
    pixels_[pixelIndex(px, py)] = colour;
}

void Canvas::addPixel(int px, int py, Rgb colour) noexcept {
    if (!containsPixel(px, py)) {
        return;
    }
    Rgb& target = pixels_[pixelIndex(px, py)];
    target = target.plus(colour);
}

Rgb Canvas::pixelAt(int px, int py) const noexcept {
    return containsPixel(px, py) ? pixels_[pixelIndex(px, py)] : kBlack;
}

namespace {

// Cohen-Sutherland region codes.
constexpr int kInside = 0;
constexpr int kLeft   = 1;
constexpr int kRight  = 2;
constexpr int kBelow  = 4;
constexpr int kAbove  = 8;

[[nodiscard]] int regionCode(double x, double y, double maxX, double maxY) noexcept {
    int code = kInside;
    if (x < 0.0)       code |= kLeft;
    else if (x > maxX) code |= kRight;
    if (y < 0.0)       code |= kAbove;
    else if (y > maxY) code |= kBelow;
    return code;
}

/// Trim a segment to the viewport in place, Cohen-Sutherland region codes;
/// returns false if it misses entirely. Clipping is the renderer's job —
/// callers never pre-cull — and afterwards Bresenham can run unbounded.
[[nodiscard]] bool clipToViewport(double& x0, double& y0, double& x1, double& y1,
                                 double maxX, double maxY) noexcept {
    int code0 = regionCode(x0, y0, maxX, maxY);
    int code1 = regionCode(x1, y1, maxX, maxY);

    // Each iteration clears at least one of the four region bits; the cap is
    // belt and braces against a NaN comparison that never converges.
    for (int guard = 0; guard < 8; ++guard) {
        if ((code0 | code1) == kInside) {
            return true;                       // wholly inside
        }
        if ((code0 & code1) != 0) {
            return false;                      // wholly outside one edge
        }

        const int outside = code0 != kInside ? code0 : code1;
        double x = 0.0;
        double y = 0.0;

        if ((outside & kBelow) != 0) {
            x = x0 + (x1 - x0) * (maxY - y0) / (y1 - y0);
            y = maxY;
        } else if ((outside & kAbove) != 0) {
            x = x0 + (x1 - x0) * (0.0 - y0) / (y1 - y0);
            y = 0.0;
        } else if ((outside & kRight) != 0) {
            y = y0 + (y1 - y0) * (maxX - x0) / (x1 - x0);
            x = maxX;
        } else {
            y = y0 + (y1 - y0) * (0.0 - x0) / (x1 - x0);
            x = 0.0;
        }

        if (!std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }

        if (outside == code0) {
            x0 = x;
            y0 = y;
            code0 = regionCode(x0, y0, maxX, maxY);
        } else {
            x1 = x;
            y1 = y;
            code1 = regionCode(x1, y1, maxX, maxY);
        }
    }
    return false;
}

}  // namespace

void Canvas::line(int x0, int y0, int x1, int y1, Rgb colour) noexcept {
    double cx0 = static_cast<double>(x0);
    double cy0 = static_cast<double>(y0);
    double cx1 = static_cast<double>(x1);
    double cy1 = static_cast<double>(y1);

    if (!clipToViewport(cx0, cy0, cx1, cy1,
                        static_cast<double>(pixelWidth() - 1),
                        static_cast<double>(pixelHeight() - 1))) {
        return;
    }

    int ax = static_cast<int>(std::lround(cx0));
    int ay = static_cast<int>(std::lround(cy0));
    const int bx = static_cast<int>(std::lround(cx1));
    const int by = static_cast<int>(std::lround(cy1));

    // Integer-only, all-octants Bresenham: no floating point, so no platform
    // rounding difference — snapshots are compared byte for byte.
    const int dx = std::abs(bx - ax);
    const int dy = -std::abs(by - ay);
    const int sx = ax < bx ? 1 : -1;
    const int sy = ay < by ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        setPixel(ax, ay, colour);
        if (ax == bx && ay == by) {
            return;
        }
        const int doubled = 2 * error;
        if (doubled >= dy) {
            error += dy;
            ax += sx;
        }
        if (doubled <= dx) {
            error += dx;
            ay += sy;
        }
    }
}

void Canvas::disc(int cx, int cy, int radius, Rgb colour) noexcept {
    if (radius <= 0) {
        setPixel(cx, cy, colour);
        return;
    }
    // Plain dx^2 + dy^2 <= r^2. An outward "+ r" fudge admits (+-1, +-1) at
    // radius 1 and renders every small body as a solid 3x3 square.
    const int rSquared = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= rSquared) {
                setPixel(cx + dx, cy + dy, colour);
            }
        }
    }
}

void Canvas::shadedDisc(int cx, int cy, int radius, Rgb colour,
                        double lightX, double lightY, double lightZ,
                        double ambient) noexcept {
    if (radius <= 1) {
        // Too small to shade meaningfully; it would only get dimmer.
        disc(cx, cy, radius, colour);
        return;
    }

    const double length = std::sqrt(lightX * lightX + lightY * lightY + lightZ * lightZ);
    if (!(length > 0.0)) {
        disc(cx, cy, radius, colour);
        return;
    }
    const double lx = lightX / length;
    const double ly = lightY / length;
    const double lz = lightZ / length;

    const double r = static_cast<double>(radius);
    const int rSquared = radius * radius;

    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > rSquared) {
                continue;
            }
            const double nx = static_cast<double>(dx) / r;
            const double ny = static_cast<double>(dy) / r;
            const double nzSquared = 1.0 - (nx * nx + ny * ny);
            const double nz = nzSquared > 0.0 ? std::sqrt(nzSquared) : 0.0;

            const double lambert = nx * lx + ny * ly + nz * lz;
            double brightness = ambient + (1.0 - ambient) * std::max(0.0, lambert);

            // Quantised to 24 levels for bandwidth, not aesthetics: a
            // continuous gradient defeats present()'s colour-run compression
            // (48 KB/frame vs 15), and 24 steps show no visible banding.
            constexpr double kLevels = 24.0;
            brightness = std::floor(brightness * kLevels) / kLevels;

            setPixel(cx + dx, cy + dy, colour.scaled(brightness));
        }
    }
}

void Canvas::glow(int cx, int cy, int radius, Rgb colour) noexcept {
    if (radius <= 0) {
        return;
    }
    const double r = static_cast<double>(radius);
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            if (d > r) {
                continue;
            }
            // Inverse-square-ish falloff: reads as light, not a flat circle.
            const double t = 1.0 - d / r;
            addPixel(cx + dx, cy + dy, colour.scaled(t * t));
        }
    }
}

// ─── output ──────────────────────────────────────────────────────────────────

void Canvas::present(std::string& out, ColourDepth depth, BlockStyle blocks,
                     bool homeCursor) const {
    out.clear();
    if (homeCursor) {
        out += "\033[H";
    }

    // Track what the terminal currently believes, so an escape is emitted only
    // on change — 15 KB/frame instead of 200 KB on a mostly-black starfield.
    // Foreground and background validity are SEPARATE facts: a space-with-
    // background cell sets only bgKnown, and one shared flag caused the
    // stale-foreground grey-block bug (docs/QA.md).
    bool fgKnown = false;
    bool bgKnown = false;
    Rgb currentFg{};
    Rgb currentBg{};
    Rgb currentInk{};
    bool inCharacterMode = false;

    if (depth != ColourDepth::Ascii) {
        out += "\033[0m";
    }

    for (int cy = 0; cy < cellHeight_; ++cy) {
        for (int cx = 0; cx < cellWidth_; ++cx) {
            const Cell& cell = cells_[cellIndex(cx, cy)];
            Rgb top = pixels_[pixelIndex(cx, cy * 2)];
            Rgb bottom = pixels_[pixelIndex(cx, cy * 2 + 1)];

            if (blocks == BlockStyle::FullCells) {
                // One pixel per cell: average the two rows so the top == bottom
                // path emits a plain space. (Brighter-of-two would drop a
                // single-pixel orbit line entirely.)
                const Rgb mean{static_cast<std::uint8_t>((top.r + bottom.r) / 2),
                               static_cast<std::uint8_t>((top.g + bottom.g) / 2),
                               static_cast<std::uint8_t>((top.b + bottom.b) / 2)};
                top = mean;
                bottom = mean;
            }

            // A character in the cell wins over the pixels beneath it.
            if (cell.glyph != ' ') {
                if (depth == ColourDepth::Ascii) {
                    out += cell.glyph;
                    continue;
                }
                if (!inCharacterMode || !(cell.fg == currentInk)) {
                    // Reset first: leaving pixel mode must clear the half-blocks'
                    // background, or HUD text inherits a planet's colour.
                    out += "\033[0m";
                    if (depth == ColourDepth::TrueColour) {
                        appendTrueColour(out, cell.fg, /*background=*/false);
                    } else {
                        appendAnsi16(out, cell.fg, /*background=*/false);
                    }
                    currentInk = cell.fg;
                    inCharacterMode = true;
                    // The reset invalidated both pixel channels.
                    fgKnown = false;
                    bgKnown = false;
                }
                out += cell.glyph;
                continue;
            }

            if (depth == ColourDepth::Ascii) {
                // Brightness becomes density; keeps --no-colour snapshots meaningful.
                const double level = std::max(top.luminance(), bottom.luminance());
                const auto index = static_cast<std::size_t>(
                    level * static_cast<double>(kDensityRamp.size() - 1) + 0.5);
                out += kDensityRamp[std::min(index, kDensityRamp.size() - 1)];
                continue;
            }

            inCharacterMode = false;

            const auto emitColour = [&](Rgb colour, bool background) {
                if (depth == ColourDepth::TrueColour) {
                    appendTrueColour(out, colour, background);
                } else {
                    appendAnsi16(out, colour, background);
                }
            };

            if (top == bottom) {
                // Both halves the same: emit a space with a background colour —
                // one byte, not U+2580's three. Sets bgKnown ONLY; the
                // foreground is neither emitted nor known.
                if (!bgKnown || !(currentBg == top)) {
                    emitColour(top, /*background=*/true);
                    currentBg = top;
                    bgKnown = true;
                }
                out += ' ';
                continue;
            }

            if (!fgKnown || !(currentFg == top)) {
                emitColour(top, /*background=*/false);
                currentFg = top;
                fgKnown = true;
            }
            if (!bgKnown || !(currentBg == bottom)) {
                emitColour(bottom, /*background=*/true);
                currentBg = bottom;
                bgKnown = true;
            }
            out += "▀";       // upper half block
        }

        if (depth != ColourDepth::Ascii) {
            // Reset before the newline, or the last background colour stretches
            // across the row. A reset clears BOTH channels: both flags drop.
            out += "\033[0m";
            fgKnown = false;
            bgKnown = false;
            inCharacterMode = false;
        }
        if (homeCursor) {
            out += "\033[K";
        }
        if (cy + 1 < cellHeight_) {
            out += homeCursor ? "\r\n" : "\n";
        }
    }

    if (depth != ColourDepth::Ascii) {
        out += "\033[0m";
    }
}

}  // namespace omma::render
