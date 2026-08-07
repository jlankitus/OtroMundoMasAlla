// ─────────────────────────────────────────────────────────────────────────────
// Canvas — a character grid you draw into, then present in one write.
//
// This is double buffering, and it exists for the same reason a graphics
// engine does it: never let the user see a partially drawn frame. Writing
// directly to stdout as you compute means the terminal shows the Sun, then a
// pause, then Mercury, then a pause — which reads as flicker. Composing the
// whole frame in memory and emitting it as a single write shows one complete
// picture per frame.
//
// The other half of the trick is not clearing. Clearing the screen and
// redrawing guarantees a moment when the screen genuinely is blank. Instead we
// home the cursor and overwrite every cell, so there is never a blank state to
// catch.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace omma::render {

/// The eight ANSI colours plus their bright variants, which is every colour
/// guaranteed to work in every terminal including ancient ones over ssh.
/// 256-colour and truecolour are prettier and not universally available; a
/// renderer that degrades to garbage on someone's terminal is worse than one
/// that looks slightly flatter everywhere.
enum class Ink : std::uint8_t {
    Default = 0,
    Black, Red, Green, Yellow, Blue, Magenta, Cyan, White,
    BrightBlack, BrightRed, BrightGreen, BrightYellow,
    BrightBlue, BrightMagenta, BrightCyan, BrightWhite,
};

class Canvas {
public:
    Canvas(int width, int height);

    void resize(int width, int height);

    /// Reset every cell to a space in the default colour.
    void clear();

    /// Draw one character. Out-of-bounds coordinates are silently ignored,
    /// which is deliberate: a renderer that has to bounds-check before every
    /// plot is a renderer full of bounds checks, and clipping is the whole
    /// point of a viewport.
    void put(int x, int y, char glyph, Ink ink = Ink::Default) noexcept;

    /// Draw a string left-to-right from (x, y). Clipped at the right edge.
    void text(int x, int y, std::string_view s, Ink ink = Ink::Default) noexcept;

    /// Fill a rectangle. Used to clear a background strip before drawing HUD
    /// text over a busy scene — without it, orbit dots show through between
    /// the letters and the readout becomes unreadable.
    void fill(int x, int y, int width, int height,
              char glyph = ' ', Ink ink = Ink::Default) noexcept;

    /// True if every cell in the horizontal span is empty. Label placement
    /// uses this to refuse to overwrite something already drawn.
    [[nodiscard]] bool spanIsClear(int x, int y, int length) const noexcept;

    /// True if the coordinate is inside the canvas.
    [[nodiscard]] bool contains(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    /// What is currently drawn at a cell, or ' ' if out of bounds. Used to
    /// avoid overdrawing a planet with an orbit trail.
    [[nodiscard]] char glyphAt(int x, int y) const noexcept;

    /// Compose the whole frame into \p out, ready for a single write.
    ///
    /// Colour escapes are emitted only when the colour changes, which for a
    /// mostly-black starfield cuts the frame size by an order of magnitude.
    /// The buffer is reused across frames by the caller so that a 60 Hz
    /// render loop performs no allocations at all in steady state.
    ///
    /// \param colour     emit SGR colour escapes. Off when piping to a file.
    /// \param homeCursor emit the cursor-home escape first. Off for a one-shot
    ///                   snapshot, which should be plain text you can diff.
    void present(std::string& out, bool colour = true, bool homeCursor = true) const;

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

private:
    struct Cell {
        char glyph{' '};
        Ink  ink{Ink::Default};
    };

    int width_;
    int height_;
    std::vector<Cell> cells_;
};

}  // namespace omma::render
