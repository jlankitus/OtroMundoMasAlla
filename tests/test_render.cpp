// Tests for the canvas and camera.
//
// Rendering code has a reputation for being untestable, which is mostly a
// consequence of tangling the drawing with the thing being drawn. Split the
// projection and the grid out into their own types and both become ordinary
// pure functions with ordinary assertions.
//
// These catch a specific class of bug that is otherwise very hard to pin down,
// because the symptom is "it looks a bit off" and the reflex is to blame the
// terminal.

#include "core/units.hpp"
#include "render/camera.hpp"
#include "render/canvas.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <string>

using Catch::Matchers::WithinRel;
using omma::Vec3;
using omma::render::Camera;
using omma::render::Canvas;
using omma::render::Ink;

// ─────────────────────────────────────────────────────────────────────────────
// Canvas
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a fresh canvas is blank", "[render][canvas]") {
    Canvas canvas{20, 5};
    REQUIRE(canvas.width() == 20);
    REQUIRE(canvas.height() == 5);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 20; ++x) {
            REQUIRE(canvas.glyphAt(x, y) == ' ');
        }
    }
}

TEST_CASE("out-of-bounds drawing is silently clipped", "[render][canvas]") {
    // Deliberate policy, not laziness. A renderer whose every plot call must
    // be guarded by the caller is a renderer full of duplicated guards, and
    // clipping is what a viewport is for. The contract is that nothing outside
    // the canvas is written and nothing crashes.
    Canvas canvas{10, 3};

    canvas.put(-1, 0, 'X');
    canvas.put(0, -1, 'X');
    canvas.put(10, 0, 'X');
    canvas.put(0, 3, 'X');
    canvas.put(1'000'000, 1'000'000, 'X');

    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 10; ++x) {
            REQUIRE(canvas.glyphAt(x, y) == ' ');
        }
    }
    REQUIRE(canvas.glyphAt(-1, 0) == ' ');    // reading out of bounds is safe too
    REQUIRE(canvas.glyphAt(50, 50) == ' ');
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
    // The rest fell off the right edge without touching the next row.
    REQUIRE(canvas.glyphAt(0, 1) == ' ');
}

TEST_CASE("fill clears a rectangle and nothing outside it", "[render][canvas]") {
    Canvas canvas{10, 5};
    canvas.text(0, 2, "##########");
    canvas.fill(2, 2, 4, 1);

    REQUIRE(canvas.glyphAt(1, 2) == '#');
    REQUIRE(canvas.glyphAt(2, 2) == ' ');
    REQUIRE(canvas.glyphAt(5, 2) == ' ');
    REQUIRE(canvas.glyphAt(6, 2) == '#');
}

TEST_CASE("spanIsClear reports room for a label", "[render][canvas]") {
    Canvas canvas{20, 3};
    canvas.put(5, 1, 'E');

    REQUIRE(canvas.spanIsClear(0, 1, 5));
    REQUIRE_FALSE(canvas.spanIsClear(3, 1, 5));    // overlaps the 'E'
    REQUIRE(canvas.spanIsClear(6, 1, 5));
    REQUIRE_FALSE(canvas.spanIsClear(18, 1, 5));   // would run off the edge
    REQUIRE_FALSE(canvas.spanIsClear(0, 9, 5));    // row does not exist
}

TEST_CASE("clear resets everything", "[render][canvas]") {
    Canvas canvas{6, 2};
    canvas.text(0, 0, "abcdef", Ink::Red);
    canvas.clear();
    REQUIRE(canvas.glyphAt(0, 0) == ' ');
    REQUIRE(canvas.glyphAt(5, 0) == ' ');
}

TEST_CASE("present renders the grid with no colour escapes when asked",
          "[render][canvas]") {
    Canvas canvas{5, 2};
    canvas.text(0, 0, "ab", Ink::Red);
    canvas.text(0, 1, "cd", Ink::Green);

    std::string out;
    canvas.present(out, /*colour=*/false, /*homeCursor=*/false);

    // Trailing spaces are trimmed: they carry no information and, over a slow
    // link at 30 frames a second, cost real bandwidth.
    REQUIRE(out == "ab\ncd");
    REQUIRE(out.find('\033') == std::string::npos);
}

TEST_CASE("present emits a colour escape only when the colour changes",
          "[render][canvas]") {
    Canvas canvas{6, 1};
    canvas.text(0, 0, "aaa", Ink::Red);
    canvas.text(3, 0, "bbb", Ink::Red);   // same ink: must not re-emit

    std::string out;
    canvas.present(out, /*colour=*/true, /*homeCursor=*/false);

    // One escape to enter default, one to enter red, one to reset at the end.
    std::size_t escapes = 0;
    for (const char c : out) {
        if (c == '\033') ++escapes;
    }
    REQUIRE(escapes == 3);
}

TEST_CASE("resize produces a blank canvas of the new size", "[render][canvas]") {
    Canvas canvas{4, 2};
    canvas.text(0, 0, "xxxx");
    canvas.resize(10, 4);
    REQUIRE(canvas.width() == 10);
    REQUIRE(canvas.height() == 4);
    REQUIRE(canvas.glyphAt(0, 0) == ' ');
}

TEST_CASE("degenerate sizes are clamped rather than crashing", "[render][canvas]") {
    // A terminal can report 0 columns while being resized, and a zero-sized
    // vector plus a division by the width is a crash in the middle of a frame.
    Canvas canvas{0, 0};
    REQUIRE(canvas.width() >= 1);
    REQUIRE(canvas.height() >= 1);
    canvas.resize(-5, -5);
    REQUIRE(canvas.width() >= 1);
    REQUIRE(canvas.height() >= 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("the centre of the world projects to the centre of the view",
          "[render][camera]") {
    Camera camera{101, 41};
    camera.setCentre(Vec3{1.0e11, -2.0e11, 0.0});
    camera.setScale(1.0e9);

    const auto p = camera.project(Vec3{1.0e11, -2.0e11, 0.0});
    REQUIRE(p.onScreen);
    REQUIRE(p.x == 50);
    REQUIRE(p.y == 20);
}

TEST_CASE("world +y is screen up", "[render][camera]") {
    // Screen rows increase downward and charts put +y at the top, so exactly
    // one sign flip is required. Getting it wrong yields a solar system that
    // orbits clockwise instead of anticlockwise, which is easy to miss and
    // impossible to unsee.
    Camera camera{101, 41};
    camera.setScale(1.0e9);

    const auto up = camera.project(Vec3{0.0, 10.0e9, 0.0});
    const auto down = camera.project(Vec3{0.0, -10.0e9, 0.0});
    REQUIRE(up.y < 20);
    REQUIRE(down.y > 20);

    const auto right = camera.project(Vec3{10.0e9, 0.0, 0.0});
    REQUIRE(right.x > 50);
}

TEST_CASE("cell aspect is corrected so circles stay round", "[render][camera]") {
    // The one genuinely non-obvious rule in the projection. A terminal cell is
    // about twice as tall as it is wide, so the same distance travelled
    // horizontally must span twice as many cells as vertically. Without this,
    // every orbit renders as an egg.
    Camera camera{201, 101};
    camera.setScale(1.0e9);

    const double d = 20.0e9;
    const auto horizontal = camera.project(Vec3{d, 0.0, 0.0});
    const auto vertical = camera.project(Vec3{0.0, d, 0.0});

    const int cellsRight = horizontal.x - 100;
    const int cellsUp = 50 - vertical.y;
    REQUIRE(cellsRight == 2 * cellsUp);
}

TEST_CASE("frame fits the requested span inside the view", "[render][camera]") {
    Camera camera{120, 40};
    constexpr double kSpan = 6.0e12;
    camera.frame(kSpan);

    // Both axes must contain the span; the wider one has slack.
    REQUIRE(camera.viewWidthMetres() >= kSpan * 0.999);
    REQUIRE(camera.viewHeightMetres() >= kSpan * 0.999);

    // ...and a point at the very edge of the requested span is still on screen.
    camera.setCentre(Vec3::zero());
    REQUIRE(camera.project(Vec3{0.0, kSpan * 0.49, 0.0}).onScreen);
}

TEST_CASE("points outside the viewport are reported off-screen", "[render][camera]") {
    Camera camera{80, 24};
    camera.setScale(1.0e9);
    REQUIRE_FALSE(camera.project(Vec3{1.0e15, 0.0, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{0.0, -1.0e15, 0.0}).onScreen);
    REQUIRE(camera.project(Vec3::zero()).onScreen);
}

TEST_CASE("the projection refuses to produce undefined behaviour",
          "[render][camera]") {
    // Two real hazards. A double-to-int conversion that overflows is undefined
    // behaviour, not a big number -- so a body a light-year away must be
    // rejected before the cast. And NaN comparisons are all false, so a NaN
    // coordinate sails through every bounds check and lands somewhere
    // arbitrary in the cell array.
    Camera camera{80, 24};
    camera.setScale(1.0);

    const double huge = 1.0e300;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    REQUIRE_FALSE(camera.project(Vec3{huge, 0.0, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{nan, 0.0, 0.0}).onScreen);
    REQUIRE_FALSE(camera.project(Vec3{0.0, inf, 0.0}).onScreen);
}

TEST_CASE("zoom is clamped to a usable range", "[render][camera]") {
    // A held zoom key must not be able to drive the scale to zero, because
    // dividing by it yields infinities that then silently fail every bounds
    // check and the display simply goes blank with no error.
    Camera camera{80, 24};

    for (int i = 0; i < 500; ++i) {
        camera.zoomBy(0.5);
    }
    REQUIRE(camera.metresPerRow() > 0.0);
    REQUIRE(std::isfinite(camera.metresPerRow()));

    for (int i = 0; i < 500; ++i) {
        camera.zoomBy(2.0);
    }
    REQUIRE(std::isfinite(camera.metresPerRow()));

    // And a non-finite scale is refused outright rather than stored.
    const double before = camera.metresPerRow();
    camera.setScale(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(camera.metresPerRow() == before);
}

TEST_CASE("a circle projects to something round", "[render][camera]") {
    // The end-to-end version of the aspect test: project a real circle and
    // check the drawn extent is as wide as it is tall, in cells, after
    // accounting for the 2:1 cell shape.
    Camera camera{161, 81};
    camera.setCentre(Vec3::zero());
    constexpr double kRadius = 1.0e11;
    camera.frame(kRadius * 2.2);

    int minX = 10'000, maxX = -10'000, minY = 10'000, maxY = -10'000;
    for (int i = 0; i < 720; ++i) {
        const double theta = omma::constants::kTwoPi * static_cast<double>(i) / 720.0;
        const auto p = camera.project(Vec3{kRadius * std::cos(theta),
                                           kRadius * std::sin(theta), 0.0});
        REQUIRE(p.onScreen);
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }

    const double widthCells = static_cast<double>(maxX - minX);
    const double heightCells = static_cast<double>(maxY - minY);
    REQUIRE_THAT(widthCells / heightCells, WithinRel(2.0, 0.02));
}
