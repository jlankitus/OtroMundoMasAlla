#include "render/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace omma::render {
namespace {

/// SGR foreground escape for each Ink. Index matches the enum.
constexpr std::string_view kInkCodes[] = {
    "\033[0m",
    "\033[30m", "\033[31m", "\033[32m", "\033[33m",
    "\033[34m", "\033[35m", "\033[36m", "\033[37m",
    "\033[90m", "\033[91m", "\033[92m", "\033[93m",
    "\033[94m", "\033[95m", "\033[96m", "\033[97m",
};
static_assert(std::size(kInkCodes) == 17, "one code per Ink value");

/// Density ramp for the no-colour presentation, darkest first. Chosen for even
/// perceived steps in a typical monospace font rather than for ASCII order —
/// ':' really does read lighter than '-' at terminal sizes.
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

/// "\033[38;2;R;G;Bm" or the background equivalent, built by hand.
///
/// snprintf would be clearer and is about eight times slower. This runs up to
/// twice per cell, twenty thousand times a frame, thirty times a second —
/// which is exactly the situation where hand-rolling the formatting is
/// justified, and the only kind of situation where it is.
void appendTrueColour(std::string& out, Rgb colour, bool background) {
    out += background ? "\033[48;2;" : "\033[38;2;";
    appendUnsigned(out, colour.r);
    out += ';';
    appendUnsigned(out, colour.g);
    out += ';';
    appendUnsigned(out, colour.b);
    out += 'm';
}

/// Nearest of the sixteen ANSI colours, for terminals without 24-bit support.
///
/// Quantises each channel to a 0/1 bit plus a brightness bit. Crude, and
/// entirely adequate: the alternative is a nearest-neighbour search through a
/// palette table for every pixel of every frame, to choose between sixteen
/// colours that are themselves whatever the user's theme says they are.
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

void Canvas::put(int cx, int cy, char glyph, Ink ink) noexcept {
    if (!contains(cx, cy)) {
        return;
    }
    cells_[cellIndex(cx, cy)] = Cell{glyph, ink};
}

void Canvas::text(int cx, int cy, std::string_view s, Ink ink) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        put(cx + static_cast<int>(i), cy, s[i], ink);
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

/// Trim a segment to the viewport, in place. Returns false if it misses
/// entirely.
///
/// WHY CLIPPING AND NOT AN ITERATION BUDGET
/// The first version just capped how many Bresenham steps a segment could take,
/// which bounded the cost but produced the wrong picture: a chord whose two
/// endpoints are both off-screen was skipped even though it crosses the middle
/// of the view. Every orbit larger than the viewport rendered as dashes.
///
/// Clipping fixes correctness and performance at once. Cohen-Sutherland trims
/// against each edge in turn using the region codes, so afterwards the segment
/// is guaranteed to lie inside and Bresenham can run unbounded.
[[nodiscard]] bool clipToViewport(double& x0, double& y0, double& x1, double& y1,
                                 double maxX, double maxY) noexcept {
    int code0 = regionCode(x0, y0, maxX, maxY);
    int code1 = regionCode(x1, y1, maxX, maxY);

    // Bounded because each iteration eliminates at least one region bit, and
    // there are only four. The cap is belt and braces against a pathological
    // input producing a NaN comparison that never converges.
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

    // Bresenham, integer only, all-octants form: no special cases for steep
    // versus shallow, and no floating point, so there is no possibility of a
    // rounding difference between platforms showing up as a different picture.
    // That matters because snapshots are compared byte for byte.
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
    // Plain dx^2 + dy^2 <= r^2.
    //
    // An earlier version used <= r^2 + r, meaning to round the boundary
    // outward so small discs read as round rather than as diamonds. At radius 1
    // that admits (+-1, +-1), because 1 + 1 <= 1 + 1 -- so every body in the
    // scene rendered as a solid 3x3 SQUARE. The fudge was worse than the
    // problem it solved: at radius 1 the honest circle is a five-pixel plus,
    // which reads as a point, and from radius 2 up the standard test is round
    // already.
    const int rSquared = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= rSquared) {
                setPixel(cx + dx, cy + dy, colour);
            }
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
            // Inverse-square-ish falloff, which is both physically suggestive
            // and the shape that actually looks like light rather than like a
            // flat coloured circle.
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
    // when something actually changes. On a mostly-black starfield this is the
    // difference between 200 KB and 15 KB per frame.
    bool haveState = false;
    Rgb currentFg{};
    Rgb currentBg{};
    Ink currentInk = Ink::Default;
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
                // Average the two rows and treat the cell as one pixel, so the
                // top == bottom path below emits a plain space. Brighter of the
                // two would lose thin features against the background; the
                // average keeps a single-pixel orbit line visible as a dimmer
                // cell rather than dropping it entirely.
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
                if (!inCharacterMode || cell.ink != currentInk || haveState) {
                    // Reset first: leaving pixel mode means clearing whatever
                    // background colour the half-blocks left behind, otherwise
                    // HUD text inherits a planet's colour as its backdrop.
                    out += "\033[0m";
                    out += kInkCodes[static_cast<std::size_t>(cell.ink)];
                    currentInk = cell.ink;
                    inCharacterMode = true;
                    haveState = false;
                }
                out += cell.glyph;
                continue;
            }

            if (depth == ColourDepth::Ascii) {
                // No colour to work with, so brightness becomes density. This
                // is what keeps --no-colour snapshots meaningful.
                const double level = std::max(top.luminance(), bottom.luminance());
                const auto index = static_cast<std::size_t>(
                    level * static_cast<double>(kDensityRamp.size() - 1) + 0.5);
                out += kDensityRamp[std::min(index, kDensityRamp.size() - 1)];
                continue;
            }

            inCharacterMode = false;

            if (top == bottom) {
                // Both halves the same: paint the whole cell as background and
                // emit a space. One byte instead of the three that U+2580
                // costs in UTF-8 — and most of a space scene is empty.
                if (!haveState || !(currentBg == top)) {
                    if (depth == ColourDepth::TrueColour) {
                        appendTrueColour(out, top, /*background=*/true);
                    } else {
                        appendAnsi16(out, top, /*background=*/true);
                    }
                    currentBg = top;
                    haveState = true;
                }
                out += ' ';
                continue;
            }

            if (!haveState || !(currentFg == top)) {
                if (depth == ColourDepth::TrueColour) {
                    appendTrueColour(out, top, /*background=*/false);
                } else {
                    appendAnsi16(out, top, /*background=*/false);
                }
                currentFg = top;
            }
            if (!haveState || !(currentBg == bottom)) {
                if (depth == ColourDepth::TrueColour) {
                    appendTrueColour(out, bottom, /*background=*/true);
                } else {
                    appendAnsi16(out, bottom, /*background=*/true);
                }
                currentBg = bottom;
            }
            haveState = true;
            out += "▀";       // upper half block
        }

        if (depth != ColourDepth::Ascii) {
            // Reset before the newline. Without this the last cell's background
            // colour stretches across the rest of the terminal row.
            out += "\033[0m";
            haveState = false;
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
