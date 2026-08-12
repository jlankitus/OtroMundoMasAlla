// Tests for the canvas and camera.
//
// Rendering code has a reputation for being untestable, which is mostly a
// consequence of tangling the drawing with the thing being drawn. Split the
// projection and the grid into their own types and both become ordinary pure
// functions with ordinary assertions.
//
// These catch a class of bug that is otherwise very hard to pin down, because
// the symptom is "it looks a bit off" and the reflex is to blame the terminal.

#include "core/units.hpp"
#include "render/camera.hpp"
#include "render/canvas.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using omma::Vec3;
using omma::render::Camera;
using omma::render::Canvas;
using omma::render::BlockStyle;
using omma::render::ColourDepth;
using omma::render::Ink;
using omma::render::Rgb;

// ─────────────────────────────────────────────────────────────────────────────
// Rgb
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("luminance uses perceptual weights, not a flat average",
          "[render][rgb]") {
    // Rec. 601: green dominates because the eye is most sensitive to it. A flat
    // average makes pure blue look as bright as pure green, and a blue planet
    // then wins a "is this pixel bright enough to hide a label" test it should
    // lose.
    const double green = Rgb{0, 255, 0}.luminance();
    const double red = Rgb{255, 0, 0}.luminance();
    const double blue = Rgb{0, 0, 255}.luminance();

    REQUIRE(green > red);
    REQUIRE(red > blue);
    REQUIRE_THAT((Rgb{255, 255, 255}.luminance()), WithinAbs(1.0, 1e-9));
    REQUIRE_THAT((Rgb{0, 0, 0}.luminance()), WithinAbs(0.0, 1e-12));
}

TEST_CASE("scale and add saturate rather than wrap", "[render][rgb]") {
    // Wrapping is the classic 8-bit colour bug: a glow that overflows turns
    // bright yellow into dark red, so the brightest part of the Sun looks like
    // a hole. Saturation is the only acceptable behaviour here.
    REQUIRE(Rgb{200, 200, 200}.scaled(2.0) == Rgb{255, 255, 255});
    REQUIRE(Rgb{200, 100, 50}.scaled(0.0) == Rgb{0, 0, 0});
    REQUIRE(Rgb{200, 100, 50}.scaled(-5.0) == Rgb{0, 0, 0});
    REQUIRE(Rgb{200, 200, 200}.plus(Rgb{100, 100, 100}) == Rgb{255, 255, 255});
    REQUIRE(Rgb{10, 20, 30}.plus(Rgb{5, 5, 5}) == Rgb{15, 25, 35});
}

// ─────────────────────────────────────────────────────────────────────────────
// Canvas — character layer
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a fresh canvas is blank in both layers", "[render][canvas]") {
    Canvas canvas{20, 5};
    REQUIRE(canvas.width() == 20);
    REQUIRE(canvas.height() == 5);
    REQUIRE(canvas.pixelWidth() == 20);
    REQUIRE(canvas.pixelHeight() == 10);   // two pixel rows per character cell

    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            REQUIRE(canvas.glyphAt(x, y) == ' ');
        }
    }
    for (int y = 0; y < canvas.pixelHeight(); ++y) {
        for (int x = 0; x < canvas.pixelWidth(); ++x) {
            REQUIRE(canvas.pixelAt(x, y) == omma::render::kBlack);
        }
    }
}

TEST_CASE("out-of-bounds drawing is silently clipped", "[render][canvas]") {
    // Deliberate policy, not laziness. A renderer whose every call must be
    // guarded by the caller is a renderer full of duplicated guards, and
    // clipping is what a viewport is for. The contract is that nothing outside
    // the canvas is written and nothing crashes.
    Canvas canvas{10, 3};

    canvas.put(-1, 0, 'X');
    canvas.put(0, -1, 'X');
    canvas.put(10, 0, 'X');
    canvas.put(0, 3, 'X');
    canvas.put(1'000'000, 1'000'000, 'X');
    canvas.setPixel(-5, -5, Rgb{255, 255, 255});
    canvas.setPixel(10, 6, Rgb{255, 255, 255});

    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 10; ++x) {
            REQUIRE(canvas.glyphAt(x, y) == ' ');
        }
    }
    REQUIRE(canvas.glyphAt(-1, 0) == ' ');
    REQUIRE(canvas.pixelAt(-1, 0) == omma::render::kBlack);
    REQUIRE(canvas.pixelAt(99, 99) == omma::render::kBlack);
}

TEST_CASE("text is drawn left to right and clipped at the edge", "[render][canvas]") {
    Canvas canvas{8, 2};
    canvas.text(0, 0, "hello");
    REQUIRE(canvas.glyphAt(0, 0) == 'h');
    REQUIRE(canvas.glyphAt(4, 0) == 'o');
    REQUIRE(canvas.glyphAt(5, 0) == ' ');

    canvas.text(6, 1, "overflow");
    REQUIRE(canvas.glyphAt(6, 1) == 'o');
    REQUIRE(canvas.glyphAt(7, 1) == 'v');
    REQUIRE(canvas.glyphAt(0, 1) == ' ');   // did not wrap to the next row
}

TEST_CASE("fill clears both layers of a rectangle", "[render][canvas]") {
    // The HUD relies on this: clearing only the characters would leave planet
    // pixels showing through between the letters.
    Canvas canvas{10, 5};
    canvas.text(0, 2, "##########");
    for (int px = 0; px < 10; ++px) {
        canvas.setPixel(px, 4, Rgb{255, 255, 255});
        canvas.setPixel(px, 5, Rgb{255, 255, 255});
    }

    canvas.fill(2, 2, 4, 1);

    REQUIRE(canvas.glyphAt(1, 2) == '#');
    REQUIRE(canvas.glyphAt(2, 2) == ' ');
    REQUIRE(canvas.glyphAt(5, 2) == ' ');
    REQUIRE(canvas.glyphAt(6, 2) == '#');
    REQUIRE(canvas.pixelAt(1, 4) == Rgb{255, 255, 255});
    REQUIRE(canvas.pixelAt(2, 4) == omma::render::kBlack);
    REQUIRE(canvas.pixelAt(2, 5) == omma::render::kBlack);
    REQUIRE(canvas.pixelAt(6, 4) == Rgb{255, 255, 255});
}

TEST_CASE("spanIsClear refuses room over glyphs AND over bright pixels",
          "[render][canvas]") {
    Canvas canvas{20, 3};
    canvas.put(5, 1, 'E');
    canvas.setPixel(12, 2, Rgb{240, 240, 240});   // cell (12,1), upper pixel row

    REQUIRE(canvas.spanIsClear(0, 1, 5));
    REQUIRE_FALSE(canvas.spanIsClear(3, 1, 5));    // overlaps the 'E'
    REQUIRE_FALSE(canvas.spanIsClear(10, 1, 5));   // overlaps the lit pixel
    REQUIRE_FALSE(canvas.spanIsClear(18, 1, 5));   // would run off the edge
    REQUIRE_FALSE(canvas.spanIsClear(0, 9, 5));    // row does not exist

    SECTION("a dim pixel does not block a label") {
        // Orbit trails are dim on purpose so labels can still sit over them;
        // only bodies are bright enough to claim the space.
        Canvas dim{20, 3};
        dim.setPixel(4, 2, Rgb{12, 12, 12});
        REQUIRE(dim.spanIsClear(2, 1, 6));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Canvas — pixel layer
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("addPixel accumulates, setPixel replaces", "[render][canvas]") {
    Canvas canvas{4, 2};
    canvas.setPixel(0, 0, Rgb{10, 10, 10});
    canvas.setPixel(0, 0, Rgb{20, 0, 0});
    REQUIRE(canvas.pixelAt(0, 0) == Rgb{20, 0, 0});

    canvas.addPixel(0, 0, Rgb{5, 5, 5});
    REQUIRE(canvas.pixelAt(0, 0) == Rgb{25, 5, 5});
}

TEST_CASE("line hits both endpoints and stays connected", "[render][canvas]") {
    Canvas canvas{40, 20};
    const Rgb white{255, 255, 255};

    canvas.line(2, 3, 30, 17, white);
    REQUIRE(canvas.pixelAt(2, 3) == white);
    REQUIRE(canvas.pixelAt(30, 17) == white);

    SECTION("no gaps: every column between the ends has at least one lit pixel") {
        // This is what "reads as a curve rather than as dashes" means, made
        // testable. A shallow line that skipped columns would be visibly dotted.
        for (int x = 2; x <= 30; ++x) {
            bool anyLit = false;
            for (int y = 0; y < canvas.pixelHeight(); ++y) {
                if (!(canvas.pixelAt(x, y) == omma::render::kBlack)) {
                    anyLit = true;
                    break;
                }
            }
            INFO("column " << x);
            REQUIRE(anyLit);
        }
    }

    SECTION("a steep line has no gaps by row either") {
        Canvas steep{40, 20};
        steep.line(5, 1, 8, 38, white);
        for (int y = 1; y <= 38; ++y) {
            bool anyLit = false;
            for (int x = 0; x < steep.pixelWidth(); ++x) {
                if (!(steep.pixelAt(x, y) == omma::render::kBlack)) {
                    anyLit = true;
                    break;
                }
            }
            INFO("row " << y);
            REQUIRE(anyLit);
        }
    }
}

TEST_CASE("a chord whose BOTH endpoints are off-screen still gets drawn",
          "[render][canvas][regression]") {
    // The bug that made every orbit larger than the viewport render as dashes.
    //
    // Both endpoints of this segment are outside the canvas, but the segment
    // passes straight through the middle of it. The original code skipped any
    // segment with no on-screen endpoint, and an ellipse wider than the view
    // consists almost entirely of such chords -- so all that survived were a few
    // fragments near the edges.
    Canvas canvas{40, 20};
    canvas.line(-50, 20, 90, 20, Rgb{255, 255, 255});

    int lit = 0;
    for (int x = 0; x < canvas.pixelWidth(); ++x) {
        if (!(canvas.pixelAt(x, 20) == omma::render::kBlack)) {
            ++lit;
        }
    }
    INFO("lit pixels along the crossing row: " << lit);
    REQUIRE(lit == canvas.pixelWidth());   // the whole row, edge to edge
}

TEST_CASE("clipping keeps a diagonal chord continuous", "[render][canvas][regression]") {
    // The diagonal case, where a naive clip that only adjusts one endpoint
    // leaves a gap or a kink.
    Canvas canvas{60, 30};
    canvas.line(-100, -100, 160, 160, Rgb{255, 255, 255});

    // Every row the diagonal crosses must have at least one lit pixel.
    for (int y = 0; y < canvas.pixelHeight(); ++y) {
        bool anyLit = false;
        for (int x = 0; x < canvas.pixelWidth(); ++x) {
            if (!(canvas.pixelAt(x, y) == omma::render::kBlack)) {
                anyLit = true;
                break;
            }
        }
        INFO("row " << y);
        REQUIRE(anyLit);
    }
}

TEST_CASE("a segment entirely outside the viewport draws nothing",
          "[render][canvas]") {
    Canvas canvas{20, 10};
    canvas.line(-50, -50, -10, -10, Rgb{255, 0, 0});
    canvas.line(100, 5, 200, 8, Rgb{255, 0, 0});

    for (int y = 0; y < canvas.pixelHeight(); ++y) {
        for (int x = 0; x < canvas.pixelWidth(); ++x) {
            REQUIRE(canvas.pixelAt(x, y) == omma::render::kBlack);
        }
    }
}

TEST_CASE("a line from far off-screen terminates", "[render][canvas]") {
    // Orbits are drawn from projected coordinates, which can legitimately be
    // enormous when zoomed in. Without an iteration budget, Bresenham would
    // spend millions of steps plotting nothing and the frame rate would fall
    // off a cliff at high zoom for no visible reason.
    Canvas canvas{20, 10};
    canvas.line(-500'000, -500'000, 500'000, 500'000, Rgb{255, 0, 0});
    SUCCEED("returned rather than looping for millions of iterations");
}

TEST_CASE("disc always lights at least one pixel", "[render][canvas]") {
    // A body whose apparent radius rounds to zero must still be visible.
    // Without this floor, Earth vanishes at any zoom wider than the lunar
    // orbit: correct, and useless.
    Canvas canvas{20, 10};
    canvas.disc(10, 10, 0, Rgb{255, 255, 255});
    REQUIRE(canvas.pixelAt(10, 10) == Rgb{255, 255, 255});

    SECTION("and a radius-3 disc is round, not diamond") {
        Canvas big{40, 20};
        big.disc(20, 20, 3, Rgb{255, 255, 255});
        // The cardinal extremes and the diagonals should both be filled.
        REQUIRE_FALSE(big.pixelAt(23, 20) == omma::render::kBlack);
        REQUIRE_FALSE(big.pixelAt(20, 23) == omma::render::kBlack);
        REQUIRE_FALSE(big.pixelAt(22, 22) == omma::render::kBlack);
        REQUIRE(big.pixelAt(24, 24) == omma::render::kBlack);   // outside
    }
}

TEST_CASE("a radius-1 disc is a plus, not a 3x3 square",
          "[render][canvas][regression]") {
    // The bug that made every body in the scene look like a chunky block.
    //
    // disc() used dx^2 + dy^2 <= r^2 + r, intending to round the boundary
    // outward so small discs read as round rather than as diamonds. At radius 1
    // that admits (+-1, +-1), because 1 + 1 <= 1 + 1 -- a solid 3x3 square. And
    // radius 1 is what almost every body gets at almost every zoom, so the
    // entire solar system rendered as squares.
    Canvas canvas{20, 10};
    canvas.disc(10, 10, 1, Rgb{255, 255, 255});

    const auto lit = [&](int x, int y) {
        return !(canvas.pixelAt(x, y) == omma::render::kBlack);
    };

    REQUIRE(lit(10, 10));                                  // centre
    REQUIRE(lit(9, 10)); REQUIRE(lit(11, 10));             // horizontal arms
    REQUIRE(lit(10, 9)); REQUIRE(lit(10, 11));             // vertical arms
    REQUIRE_FALSE(lit(9, 9));                              // corners stay dark
    REQUIRE_FALSE(lit(11, 9));
    REQUIRE_FALSE(lit(9, 11));
    REQUIRE_FALSE(lit(11, 11));
}

TEST_CASE("a half block always gets an explicit foreground",
          "[render][canvas][regression]") {
    // The bug that scattered light grey blocks along every orbit, and moved them
    // around whenever the window was resized.
    //
    // A '▀' paints its top pixel with the terminal's CURRENT foreground. Three
    // cells in sequence were enough to break it:
    //
    //   cell 0  a character  -> emits "\033[0m" + its ink. The reset changes the
    //                           terminal's foreground to the HUD colour.
    //   cell 1  halves equal  -> emitted as a SPACE with a background only. A
    //                           space never touches the foreground -- but the old
    //                           code set a single `haveState` flag here, claiming
    //                           BOTH channels were now known.
    //   cell 2  halves differ -> its top pixel happened to match the stale
    //                           `currentFg`, so the foreground escape was
    //                           skipped, and the block rendered in the HUD's grey.
    //
    // Foreground and background validity are separate facts and needed separate
    // flags. Invisible to every other test here, because it only appears in a
    // composed frame; found by auditing the byte stream of a real render.
    Canvas canvas{3, 1};
    canvas.put(0, 0, 'X', Ink::BrightBlack);
    // Cell 1 stays black in both halves -> the space path.
    // Cell 2 differs -> the half-block path, top pixel black like the stale value.
    canvas.setPixel(2, 1, Rgb{200, 40, 40});

    std::string out;
    canvas.present(out, ColourDepth::TrueColour, BlockStyle::HalfBlocks, false);

    const std::size_t blockAt = out.find("\xE2\x96\x80");
    REQUIRE(blockAt != std::string::npos);

    // Walk the prefix and require a foreground escape after the last reset.
    const std::string prefix = out.substr(0, blockAt);
    const std::size_t lastReset = prefix.rfind("\033[0m");
    REQUIRE(lastReset != std::string::npos);
    const std::string sinceReset = prefix.substr(lastReset + 4);

    INFO("bytes since the last reset: " << sinceReset);
    REQUIRE(sinceReset.find("\033[38;2;") != std::string::npos);
}

TEST_CASE("full-cell mode emits no multi-byte glyphs",
          "[render][canvas][regression]") {
    // The fallback for a console that cannot be put into UTF-8. Half blocks are
    // three bytes each; on code page 437 they arrive as three Latin-1 characters
    // and the display is destroyed. This mode averages the two pixel rows and
    // emits a space with a background colour instead: half the vertical
    // resolution, and it works on any terminal ever made.
    Canvas canvas{6, 2};
    canvas.setPixel(0, 0, Rgb{255, 0, 0});
    canvas.setPixel(0, 1, Rgb{0, 0, 255});      // halves differ, so half-block
                                                // mode would use U+2580 here

    std::string half;
    canvas.present(half, ColourDepth::TrueColour, BlockStyle::HalfBlocks, false);
    REQUIRE(half.find("\xE2\x96\x80") != std::string::npos);

    std::string full;
    canvas.present(full, ColourDepth::TrueColour, BlockStyle::FullCells, false);
    REQUIRE(full.find("\xE2\x96\x80") == std::string::npos);
    for (const char c : full) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);   // pure ASCII bytes
    }
    // ...and the averaged colour is there as a background.
    REQUIRE(full.find("\033[48;2;127;0;127m") != std::string::npos);
}

TEST_CASE("glow falls off with distance and never wraps", "[render][canvas]") {
    Canvas canvas{60, 30};
    canvas.glow(30, 30, 10, Rgb{200, 180, 120});

    const double centre = canvas.pixelAt(30, 30).luminance();
    const double mid = canvas.pixelAt(35, 30).luminance();
    const double edge = canvas.pixelAt(39, 30).luminance();

    REQUIRE(centre > mid);
    REQUIRE(mid > edge);
    REQUIRE(canvas.pixelAt(45, 30) == omma::render::kBlack);   // outside the radius

    SECTION("overlapping glows saturate instead of wrapping to dark") {
        for (int i = 0; i < 20; ++i) {
            canvas.glow(30, 30, 10, Rgb{200, 180, 120});
        }
        REQUIRE(canvas.pixelAt(30, 30).luminance() >= centre);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Canvas — presentation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ASCII presentation turns brightness into density", "[render][canvas]") {
    // The mode that keeps `--snapshot --no-colour` meaningful. Without it a
    // pixel-based renderer produces a blank rectangle when colour is off, and
    // the visual output stops being diffable — which is the same as it stopping
    // being tested.
    Canvas canvas{4, 1};
    canvas.setPixel(0, 0, Rgb{0, 0, 0});
    canvas.setPixel(1, 0, Rgb{60, 60, 60});
    canvas.setPixel(2, 0, Rgb{150, 150, 150});
    canvas.setPixel(3, 0, Rgb{255, 255, 255});

    std::string out;
    canvas.present(out, ColourDepth::Ascii, BlockStyle::HalfBlocks,
                   /*homeCursor=*/false);

    REQUIRE(out.size() == 4);
    REQUIRE(out.find('\033') == std::string::npos);
    REQUIRE(out[0] == ' ');
    REQUIRE(out[3] == '@');
    // Monotonically denser.
    const std::string ramp = " .:-=+*#%@";
    REQUIRE(ramp.find(out[1]) < ramp.find(out[2]));
    REQUIRE(ramp.find(out[2]) < ramp.find(out[3]));
}

TEST_CASE("characters override the pixels beneath them", "[render][canvas]") {
    Canvas canvas{3, 1};
    canvas.setPixel(0, 0, Rgb{255, 255, 255});
    canvas.setPixel(0, 1, Rgb{255, 255, 255});
    canvas.put(0, 0, 'X');

    std::string out;
    canvas.present(out, ColourDepth::Ascii, BlockStyle::HalfBlocks,
                   /*homeCursor=*/false);
    REQUIRE(out[0] == 'X');
}

TEST_CASE("true colour presentation emits half blocks only where halves differ",
          "[render][canvas]") {
    Canvas canvas{2, 1};
    // Cell 0: both halves identical -> a space with a background colour, which
    // is one byte instead of the three U+2580 costs in UTF-8. Most of a space
    // scene is empty, so this is the difference between 200 KB and 15 KB a frame.
    canvas.setPixel(0, 0, Rgb{10, 20, 30});
    canvas.setPixel(0, 1, Rgb{10, 20, 30});
    // Cell 1: halves differ -> a real half block.
    canvas.setPixel(1, 0, Rgb{200, 0, 0});
    canvas.setPixel(1, 1, Rgb{0, 0, 200});

    std::string out;
    canvas.present(out, ColourDepth::TrueColour, BlockStyle::HalfBlocks,
                   /*homeCursor=*/false);

    REQUIRE(out.find("\033[48;2;10;20;30m ") != std::string::npos);
    REQUIRE(out.find("\033[38;2;200;0;0m") != std::string::npos);
    REQUIRE(out.find("\xE2\x96\x80") != std::string::npos);   // U+2580
}

TEST_CASE("a uniform field emits one colour escape, not one per cell",
          "[render][canvas]") {
    Canvas canvas{40, 10};
    for (int y = 0; y < canvas.pixelHeight(); ++y) {
        for (int x = 0; x < canvas.pixelWidth(); ++x) {
            canvas.setPixel(x, y, Rgb{7, 8, 9});
        }
    }

    std::string out;
    canvas.present(out, ColourDepth::TrueColour, BlockStyle::HalfBlocks,
                   /*homeCursor=*/false);

    // 400 cells. Without run compression that would be 400 colour escapes; with
    // it, one per row plus the resets.
    std::size_t colourEscapes = 0;
    std::size_t at = 0;
    while ((at = out.find("\033[48;2;", at)) != std::string::npos) {
        ++colourEscapes;
        at += 7;
    }
    INFO("colour escapes emitted: " << colourEscapes);
    REQUIRE(colourEscapes <= static_cast<std::size_t>(canvas.height()) + 2);
}

TEST_CASE("resize produces a blank canvas of the new size", "[render][canvas]") {
    Canvas canvas{4, 2};
    canvas.text(0, 0, "xxxx");
    canvas.setPixel(1, 1, Rgb{255, 0, 0});
    canvas.resize(10, 4);
    REQUIRE(canvas.width() == 10);
    REQUIRE(canvas.pixelHeight() == 8);
    REQUIRE(canvas.glyphAt(0, 0) == ' ');
    REQUIRE(canvas.pixelAt(1, 1) == omma::render::kBlack);
}

TEST_CASE("degenerate sizes are clamped rather than crashing", "[render][canvas]") {
    // A terminal can report 0 columns mid-resize, and a zero-sized buffer plus
    // a division by the width is a crash in the middle of a frame.
    Canvas canvas{0, 0};
    REQUIRE(canvas.width() >= 1);
    REQUIRE(canvas.height() >= 1);
    canvas.resize(-5, -5);
    REQUIRE(canvas.width() >= 1);
    REQUIRE(canvas.height() >= 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera — projection
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("the view centre projects to the centre of the view, at any tilt",
          "[render][camera]") {
    Camera camera{101, 41};
    camera.setCentre(Vec3{1.0e11, -2.0e11, 3.0e9});
    camera.setScale(1.0e9);

    for (const double tiltDeg : {90.0, 60.0, 30.0, 5.0}) {
        for (const double spinDeg : {0.0, 45.0, 137.0, 300.0}) {
            camera.setElevation(omma::toRadians(tiltDeg));
            camera.setAzimuth(omma::toRadians(spinDeg));
            const auto p = camera.project(camera.centre());
            INFO("tilt " << tiltDeg << " spin " << spinDeg);
            REQUIRE(p.onScreen);
            REQUIRE(p.x == 50);
            REQUIRE(p.y == 20);
        }
    }
}

TEST_CASE("elevation 90 degrees reproduces the top-down projection",
          "[render][camera]") {
    // The tilted projection must collapse exactly to the old (x, y) view at
    // elevation 90. If it does not, every previously-correct picture changed
    // meaning when the tilt was added.
    Camera camera{201, 101, /*unitAspect=*/1.0};
    camera.setElevation(Camera::kTopDownElevation);
    camera.setAzimuth(0.0);
    camera.setScale(1.0e9);

    const auto right = camera.project(Vec3{10.0e9, 0.0, 0.0});
    const auto up = camera.project(Vec3{0.0, 10.0e9, 0.0});
    const auto depth = camera.project(Vec3{0.0, 0.0, 10.0e9});

    REQUIRE(right.x == 100 + 10);
    REQUIRE(right.y == 50);
    REQUIRE(up.x == 100);
    REQUIRE(up.y == 50 - 10);
    // z is the discarded axis looking straight down, so it must not move the
    // projected point at all.
    REQUIRE(depth.x == 100);
    REQUIRE(depth.y == 50);
}

TEST_CASE("world +y is screen up", "[render][camera]") {
    // Screen rows increase downward and charts put +y at the top, so exactly
    // one sign flip is required. Getting it wrong yields a solar system that
    // orbits clockwise instead of anticlockwise: easy to miss, impossible to
    // unsee.
    Camera camera{101, 41};
    camera.setElevation(Camera::kTopDownElevation);
    camera.setScale(1.0e9);

    REQUIRE(camera.project(Vec3{0.0, 10.0e9, 0.0}).y < 20);
    REQUIRE(camera.project(Vec3{0.0, -10.0e9, 0.0}).y > 20);
    REQUIRE(camera.project(Vec3{10.0e9, 0.0, 0.0}).x > 50);
}

TEST_CASE("tilting compresses the plane and reveals the third axis",
          "[render][camera]") {
    // The entire point of the feature. At 30 degrees the y axis compresses by
    // sin(30) = 0.5 exactly — which is what makes it 2:1 DIMETRIC, the
    // projection RollerCoaster Tycoon uses, rather than true isometric.
    Camera camera{201, 101, 1.0};
    camera.setAzimuth(0.0);
    camera.setScale(1.0e9);

    camera.setElevation(Camera::kTopDownElevation);
    const int flatY = 50 - camera.project(Vec3{0.0, 20.0e9, 0.0}).y;

    camera.setElevation(Camera::kDimetricElevation);
    const int tiltedY = 50 - camera.project(Vec3{0.0, 20.0e9, 0.0}).y;
    const int outOfPlaneY = 50 - camera.project(Vec3{0.0, 0.0, 20.0e9}).y;

    REQUIRE_THAT(static_cast<double>(tiltedY),
                 WithinRel(static_cast<double>(flatY) * 0.5, 0.05));
    // And z, invisible from overhead, now moves the point by cos(30) = 0.866.
    REQUIRE_THAT(static_cast<double>(outOfPlaneY),
                 WithinRel(static_cast<double>(flatY) * 0.866, 0.05));
}

TEST_CASE("a polar orbit is a line from overhead and an ellipse when tilted",
          "[render][camera]") {
    // The argument for building this at all. A sun-synchronous satellite at 98
    // degrees inclination — the workhorse of earth observation — renders as a
    // single vertical stroke from directly above and tells you nothing.
    Camera camera{201, 201, 1.0};
    camera.setCentre(Vec3::zero());
    camera.setScale(1.0e5);
    constexpr double kRadius = 7.0e6;

    const auto spread = [&](double inclination) {
        int minX = 10'000, maxX = -10'000;
        for (int i = 0; i < 360; ++i) {
            const double theta = omma::toRadians(static_cast<double>(i));
            // A circular orbit inclined about the x axis.
            const Vec3 point{kRadius * std::cos(theta),
                             kRadius * std::sin(theta) * std::cos(inclination),
                             kRadius * std::sin(theta) * std::sin(inclination)};
            const auto p = camera.project(point);
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
        }
        return maxX - minX;
    };

    // Looking straight down at a 90-degree-inclined orbit: it collapses.
    camera.setElevation(Camera::kTopDownElevation);
    camera.setAzimuth(omma::toRadians(90.0));
    const int flatSpread = [&] {
        int minY = 10'000, maxY = -10'000;
        for (int i = 0; i < 360; ++i) {
            const double theta = omma::toRadians(static_cast<double>(i));
            const Vec3 point{0.0, kRadius * std::cos(theta), kRadius * std::sin(theta)};
            const auto p = camera.project(point);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        return maxY - minY;
    }();
    REQUIRE(flatSpread <= 2);        // a stroke, not an ellipse

    // Tilt to dimetric and the same orbit acquires real vertical extent.
    camera.setElevation(Camera::kDimetricElevation);
    camera.setAzimuth(0.0);
    const int tiltedSpread = [&] {
        int minY = 10'000, maxY = -10'000;
        for (int i = 0; i < 360; ++i) {
            const double theta = omma::toRadians(static_cast<double>(i));
            const Vec3 point{0.0, kRadius * std::cos(theta), kRadius * std::sin(theta)};
            const auto p = camera.project(point);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        return maxY - minY;
    }();
    REQUIRE(tiltedSpread > 50);

    static_cast<void>(spread);
}

TEST_CASE("viewDirection is the axis the projection discards", "[render][camera]") {
    Camera camera{80, 40};

    camera.setElevation(Camera::kTopDownElevation);
    camera.setAzimuth(0.0);
    const Vec3 down = camera.viewDirection();
    REQUIRE_THAT(down.x, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(down.y, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(down.z, WithinAbs(1.0, 1e-12));

    camera.setElevation(omma::toRadians(0.02 * 180.0 / omma::constants::kPi));
    camera.setAzimuth(0.0);
    const Vec3 edgeOn = camera.viewDirection();
    REQUIRE_THAT(edgeOn.y, WithinAbs(-1.0, 0.01));

    SECTION("and it is always unit length") {
        for (const double tilt : {5.0, 30.0, 60.0, 90.0}) {
            for (const double spin : {0.0, 90.0, 217.0}) {
                camera.setElevation(omma::toRadians(tilt));
                camera.setAzimuth(omma::toRadians(spin));
                REQUIRE_THAT(camera.viewDirection().norm(), WithinAbs(1.0, 1e-12));
            }
        }
    }
}

TEST_CASE("depthOf orders points from far to near", "[render][camera]") {
    // The scalar depth buffer that lets an orbit pass behind a planet instead
    // of always over the top of it.
    Camera camera{80, 40};
    camera.setCentre(Vec3::zero());
    camera.setElevation(Camera::kTopDownElevation);

    // Looking straight down, higher z is nearer.
    REQUIRE(camera.depthOf(Vec3{0.0, 0.0, 1.0e9}) > camera.depthOf(Vec3{0.0, 0.0, -1.0e9}));
    // And a point in the plane is exactly at the centre's depth.
    REQUIRE_THAT((camera.depthOf(Vec3{5.0e9, 5.0e9, 0.0})), WithinAbs(0.0, 1.0));
}

TEST_CASE("frame fits the requested span at any tilt", "[render][camera]") {
    // Framing must mean the same thing tilted as flat. A naive implementation
    // ignores the sin(elevation) compression, so framing the system and then
    // tilting leaves the view zoomed too far out.
    Camera camera{120, 80, 1.0};
    constexpr double kSpan = 6.0e12;

    for (const double tiltDeg : {90.0, 45.0, 30.0}) {
        camera.setElevation(omma::toRadians(tiltDeg));
        camera.setCentre(Vec3::zero());
        camera.frame(kSpan);
        INFO("tilt " << tiltDeg << " metresPerRow " << camera.metresPerRow());
        REQUIRE(camera.project(Vec3{kSpan * 0.49, 0.0, 0.0}).onScreen);
        REQUIRE(camera.project(Vec3{0.0, kSpan * 0.49, 0.0}).onScreen);
    }
}

TEST_CASE("points outside the viewport are reported off-screen", "[render][camera]") {
    Camera camera{80, 24};
    camera.setScale(1.0e9);
    REQUIRE_FALSE(camera.project(Vec3{1.0e15, 0.0, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{0.0, -1.0e15, 0.0}).onScreen);
    REQUIRE(camera.project(Vec3::zero()).onScreen);
}

TEST_CASE("an unrepresentably distant point is clamped, not collapsed to the origin",
          "[render][camera][regression]") {
    // The third bug behind the dashed orbits.
    //
    // project() used to return {0, 0, false} for anything beyond its safe cast
    // range. But (0, 0) is the top-left CORNER -- a perfectly valid coordinate --
    // so a line drawn to such a point ran diagonally across the screen to the
    // corner instead of off the edge in the direction it was actually heading.
    //
    // Clamping preserves the direction, which is what a clipped line needs.
    Camera camera{80, 24};
    camera.setElevation(Camera::kTopDownElevation);
    camera.setCentre(Vec3::zero());
    camera.setScale(1.0);          // one metre per row: everything is far away

    const auto right = camera.project(Vec3{1.0e300, 0.0, 0.0});
    const auto left = camera.project(Vec3{-1.0e300, 0.0, 0.0});
    const auto up = camera.project(Vec3{0.0, 1.0e300, 0.0});

    REQUIRE(right.valid);
    REQUIRE_FALSE(right.onScreen);
    REQUIRE(right.x > 80);              // off the RIGHT edge, not at the origin
    REQUIRE(left.x < 0);                // off the LEFT edge
    REQUIRE(up.y < 0);                  // off the TOP edge

    SECTION("but a non-finite coordinate is reported invalid") {
        const auto bad = camera.project(
            Vec3{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
        REQUIRE_FALSE(bad.valid);
        REQUIRE_FALSE(bad.onScreen);
    }
}

TEST_CASE("the projection refuses to produce undefined behaviour",
          "[render][camera]") {
    // Two real hazards. A double-to-int conversion that overflows is undefined
    // behaviour, not a big number — so a body a light-year away must be
    // rejected before the cast. And NaN comparisons are all false, so a NaN
    // coordinate sails through every bounds check and lands somewhere arbitrary
    // in the pixel array.
    Camera camera{80, 24};
    camera.setScale(1.0);

    const double huge = 1.0e300;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    REQUIRE_FALSE(camera.project(Vec3{huge, 0.0, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{nan, 0.0, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{0.0, inf, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{0.0, 0.0, nan}).onScreen);
}

TEST_CASE("zoom, tilt and spin are all clamped or wrapped", "[render][camera]") {
    Camera camera{80, 24};

    for (int i = 0; i < 500; ++i) camera.zoomBy(0.5);
    REQUIRE(camera.metresPerRow() > 0.0);
    REQUIRE(std::isfinite(camera.metresPerRow()));
    for (int i = 0; i < 500; ++i) camera.zoomBy(2.0);
    REQUIRE(std::isfinite(camera.metresPerRow()));

    // Elevation must never reach zero: edge-on collapses the whole system to a
    // line, which is a legitimate projection and a useless view.
    for (int i = 0; i < 200; ++i) camera.tiltBy(-0.1);
    REQUIRE(camera.elevation() > 0.0);
    for (int i = 0; i < 200; ++i) camera.tiltBy(+0.1);
    REQUIRE(camera.elevation() <= Camera::kTopDownElevation + 1e-12);

    // Azimuth wraps, so spinning forever is fine.
    for (int i = 0; i < 1000; ++i) camera.spinBy(0.5);
    REQUIRE(camera.azimuth() >= 0.0);
    REQUIRE(camera.azimuth() < omma::constants::kTwoPi);

    const double before = camera.metresPerRow();
    camera.setScale(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(camera.metresPerRow() == before);

    const double tiltBefore = camera.elevation();
    camera.setElevation(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(camera.elevation() == tiltBefore);
}

TEST_CASE("a circle projects round at the aspect it is told about",
          "[render][camera]") {
    // The half-block canvas gives square pixels, so unitAspect is 1.0 and a
    // circle should come out as wide as it is tall. A character grid needs 2.0.
    // Passing the wrong one is the single easiest way to make every orbit an egg.
    const auto measure = [](double unitAspect) {
        Camera camera{201, 201, unitAspect};
        camera.setElevation(Camera::kTopDownElevation);
        camera.setCentre(Vec3::zero());
        constexpr double kRadius = 1.0e11;
        camera.frame(kRadius * 2.2);

        int minX = 10'000, maxX = -10'000, minY = 10'000, maxY = -10'000;
        for (int i = 0; i < 720; ++i) {
            const double theta = omma::constants::kTwoPi * static_cast<double>(i) / 720.0;
            const auto p = camera.project(Vec3{kRadius * std::cos(theta),
                                               kRadius * std::sin(theta), 0.0});
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
        return static_cast<double>(maxX - minX) / static_cast<double>(maxY - minY);
    };

    REQUIRE_THAT(measure(1.0), WithinRel(1.0, 0.02));   // square pixels
    REQUIRE_THAT(measure(2.0), WithinRel(2.0, 0.02));   // character cells
}
