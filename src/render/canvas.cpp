#include "render/canvas.hpp"

#include <algorithm>

namespace omma::render {
namespace {

/// SGR escape for each Ink. Index matches the enum.
constexpr std::string_view kInkCodes[] = {
    "\033[0m",     // Default
    "\033[30m", "\033[31m", "\033[32m", "\033[33m",
    "\033[34m", "\033[35m", "\033[36m", "\033[37m",
    "\033[90m", "\033[91m", "\033[92m", "\033[93m",
    "\033[94m", "\033[95m", "\033[96m", "\033[97m",
};

static_assert(std::size(kInkCodes) == 17, "one code per Ink value");

}  // namespace

Canvas::Canvas(int width, int height)
    : width_{std::max(1, width)}, height_{std::max(1, height)},
      cells_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)) {}

void Canvas::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    cells_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), Cell{});
}

void Canvas::clear() {
    std::fill(cells_.begin(), cells_.end(), Cell{});
}

void Canvas::put(int x, int y, char glyph, Ink ink) noexcept {
    if (!contains(x, y)) {
        return;
    }
    cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)
           + static_cast<std::size_t>(x)] = Cell{glyph, ink};
}

void Canvas::text(int x, int y, std::string_view s, Ink ink) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        put(x + static_cast<int>(i), y, s[i], ink);
    }
}

void Canvas::fill(int x, int y, int width, int height, char glyph, Ink ink) noexcept {
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            put(column, row, glyph, ink);
        }
    }
}

bool Canvas::spanIsClear(int x, int y, int length) const noexcept {
    if (y < 0 || y >= height_) {
        return false;
    }
    for (int i = 0; i < length; ++i) {
        const int column = x + i;
        if (column < 0 || column >= width_) {
            return false;      // would be clipped; treat as no room
        }
        if (glyphAt(column, y) != ' ') {
            return false;
        }
    }
    return true;
}

char Canvas::glyphAt(int x, int y) const noexcept {
    if (!contains(x, y)) {
        return ' ';
    }
    return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)
                  + static_cast<std::size_t>(x)].glyph;
}

void Canvas::present(std::string& out, bool colour, bool homeCursor) const {
    out.clear();
    if (homeCursor) {
        out += "\033[H";             // home, do not clear
    }

    Ink current = Ink::Default;
    if (colour) {
        out += kInkCodes[0];
    }

    for (int y = 0; y < height_; ++y) {
        // Trailing spaces carry no information and inflate every frame by up
        // to a full row's worth of bytes. Finding the last non-space up front
        // costs one scan and saves writing thousands of characters per frame
        // over a slow link.
        int lastInked = -1;
        for (int x = width_ - 1; x >= 0; --x) {
            if (glyphAt(x, y) != ' ') {
                lastInked = x;
                break;
            }
        }

        for (int x = 0; x <= lastInked; ++x) {
            const Cell& cell = cells_[static_cast<std::size_t>(y)
                                          * static_cast<std::size_t>(width_)
                                      + static_cast<std::size_t>(x)];
            if (colour && cell.ink != current) {
                out += kInkCodes[static_cast<std::size_t>(cell.ink)];
                current = cell.ink;
            }
            out += cell.glyph;
        }
        // Erase the rest of the line. Without this, a glyph from the previous
        // frame that is no longer drawn simply stays on screen -- the classic
        // "comet trail of stale pixels" bug you get from partial redraws.
        if (homeCursor) {
            out += "\033[K";
        }
        // No newline after the final row: it would scroll the alternate screen
        // buffer by one line and the top row would walk off the top over time.
        if (y + 1 < height_) {
            out += homeCursor ? "\r\n" : "\n";
        }
    }
    if (colour) {
        out += kInkCodes[0];
    }
}

}  // namespace omma::render
