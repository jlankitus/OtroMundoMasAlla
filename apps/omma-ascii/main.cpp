// ─────────────────────────────────────────────────────────────────────────────
// omma-ascii — the solar system, in your terminal, on a tilted board.
//
// THE GAME LOOP, and where the determinism boundary sits inside it
//
//     while running:
//         realDt = wall clock since last frame        <- nondeterministic
//         steps  = pacer.stepsForFrame(realDt, warp)
//         clock.step(steps)                           <- deterministic below here
//         handle input
//         render at clock.now()
//         sleep out the rest of the budget
//
// Everything above the clock.step() line depends on how fast this machine
// happens to be. Everything below depends only on the tick count, so a recorded
// sequence of step counts replays identically anywhere.
//
// WHY A MILLION STEPS PER FRAME COSTS NOTHING HERE
// SimClock::step(n) is one integer addition, and every body in this scene is a
// Kepler ephemeris — a pure function of time. There is no per-step work to do;
// we advance the tick count and sample once at the new epoch. Not a shortcut
// around the fixed-step discipline: the on-rails argument made concrete. The
// cost of warp is the cost of whatever is being INTEGRATED, and right now that
// is nothing. The moment a spacecraft is under thrust, maxStepsPerFrame comes
// back down to whatever the RK4 kernel can finish inside a frame.
// ─────────────────────────────────────────────────────────────────────────────

#include "bodies.hpp"

#include "core/console.hpp"
#include "core/epoch.hpp"
#include "core/sim_clock.hpp"
#include "core/units.hpp"
#include "core/version.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/solar_system.hpp"
#include "render/camera.hpp"
#include "render/canvas.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace con = omma::console;
using omma::app::BodyStyle;
using omma::app::styleFor;
using omma::BodyId;
using omma::Epoch;
using omma::SolarSystem;
using omma::constants::kAu;
using omma::constants::kSecondsPerDay;
using omma::render::Camera;
using omma::render::Canvas;
using omma::render::ColourDepth;
using omma::render::Ink;
using omma::render::Rgb;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Warp and zoom ladders. Discrete presets rather than a continuous multiplier:
// you almost always want "one day per second", not "37194 times".
// ─────────────────────────────────────────────────────────────────────────────
struct WarpStep { double factor; const char* label; };

constexpr std::array<WarpStep, 12> kWarpLadder{{
    {1.0,           "real time"},
    {60.0,          "1 min/s"},
    {600.0,         "10 min/s"},
    {3'600.0,       "1 hour/s"},
    {21'600.0,      "6 hours/s"},
    {86'400.0,      "1 day/s"},
    {604'800.0,     "1 week/s"},
    {2'629'800.0,   "1 month/s"},
    {7'889'400.0,   "3 months/s"},
    {31'557'600.0,  "1 year/s"},
    {157'788'000.0, "5 years/s"},
    {631'152'000.0, "20 years/s"},
}};

struct ZoomPreset { double metresAcross; const char* label; };

constexpr std::array<ZoomPreset, 5> kZoomPresets{{
    {1.2e9,  "Earth-Moon"},
    {3.6e11, "inner system"},
    {1.7e12, "to Jupiter"},
    {9.5e12, "to Neptune"},
    {1.6e13, "everything"},
}};

/// Where the milliseconds in a frame actually went. Press 'f' to see it.
///
/// Added because "why is this 22 fps and not 30" had two equally plausible
/// answers — console write throughput, and Windows' 15.625 ms timer tick
/// rounding the sleep up — and no way to tell them apart by reading the code.
/// Four timestamps settle it. It stays in, because the question comes back
/// every time the renderer grows.
struct FrameTimings {
    double drawMs{0.0};
    double presentMs{0.0};
    double writeMs{0.0};
    double sleepMs{0.0};
    double totalMs{0.0};
    std::size_t frameBytes{0};
};

struct ViewState {
    std::size_t warpIndex{5};        // 1 day per second
    std::size_t focusIndex{0};       // the Sun
    bool        paused{false};
    bool        showOrbits{true};
    bool        showLabels{true};
    bool        showHelp{false};
    bool        showTimings{false};
    bool        running{true};
    omma::Vec3  panOffset{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Drawing
// ─────────────────────────────────────────────────────────────────────────────

/// Draw the closed ellipse a body traces around its parent, as a polyline.
///
/// TWO CHOICES WORTH THE WORDS
///
/// Sampled in ECCENTRIC anomaly, not uniformly in time. Uniform time clusters
/// samples at apoapsis, where the body dawdles, and leaves the fast, tightly
/// curved periapsis arc full of gaps. Uniform eccentric anomaly spreads points
/// evenly around the figure, which is what you want for drawing it.
///
/// Drawn as connected line segments, not scattered points. Zoomed out, adjacent
/// samples land many pixels apart and a point cloud reads as a dashed line;
/// segments read as a curve. It also means 256 samples suffice at any zoom
/// instead of needing thousands.
void drawOrbit(Canvas& canvas, const Camera& camera,
               const omma::KeplerEphemeris& body, Epoch t, Rgb colour) {
    const auto elements = body.elementsAt(t);
    const omma::Vec3 parentOrigin =
        body.parent() != nullptr ? body.parent()->sample(t).position : omma::Vec3::zero();

    const double a = elements.semiMajorAxis;
    const double e = elements.eccentricity;
    if (!(a > 0.0) || e >= 1.0) {
        return;
    }

    // Perifocal-to-reference rotation, computed once for the whole ellipse.
    // Full 3x3 this time, not the 2x2 the top-down view got away with: a
    // tilted camera needs the z component, and that z component is the entire
    // reason inclination is visible at all.
    const double cosO = std::cos(elements.longitudeOfAscendingNode);
    const double sinO = std::sin(elements.longitudeOfAscendingNode);
    const double cosW = std::cos(elements.argumentOfPeriapsis);
    const double sinW = std::sin(elements.argumentOfPeriapsis);
    const double cosI = std::cos(elements.inclination);
    const double sinI = std::sin(elements.inclination);

    const double m11 = cosO * cosW - sinO * sinW * cosI;
    const double m12 = -cosO * sinW - sinO * cosW * cosI;
    const double m21 = sinO * cosW + cosO * sinW * cosI;
    const double m22 = -sinO * sinW + cosO * cosW * cosI;
    const double m31 = sinW * sinI;
    const double m32 = cosW * sinI;

    const double b = a * std::sqrt(1.0 - e * e);

    constexpr int kSamples = 256;
    omma::render::ScreenPoint previous{};
    bool havePrevious = false;

    for (int i = 0; i <= kSamples; ++i) {
        const double E = omma::constants::kTwoPi * static_cast<double>(i)
                       / static_cast<double>(kSamples);
        const double xp = a * (std::cos(E) - e);
        const double yp = b * std::sin(E);

        const omma::Vec3 world{parentOrigin.x + m11 * xp + m12 * yp,
                               parentOrigin.y + m21 * xp + m22 * yp,
                               parentOrigin.z + m31 * xp + m32 * yp};

        const auto point = camera.project(world);
        if (havePrevious && (point.onScreen || previous.onScreen)) {
            canvas.line(previous.x, previous.y, point.x, point.y, colour);
        }
        previous = point;
        havePrevious = true;
    }
}

/// Apparent radius of a body in pixels.
///
/// True to scale when zoomed in, clamped to a floor of one pixel so a planet
/// never vanishes just because it is small on screen. Without the floor, Earth
/// disappears entirely at any zoom wider than about the lunar orbit — correct,
/// and useless.
int apparentRadiusPixels(double bodyRadiusMetres, double metresPerPixel) {
    const double pixels = bodyRadiusMetres / metresPerPixel;
    if (!std::isfinite(pixels)) {
        return 1;
    }
    return std::clamp(static_cast<int>(pixels), 1, 200);
}

void drawScene(Canvas& canvas, Camera& camera, const SolarSystem& system,
               Epoch t, const ViewState& view) {
    const double metresPerPixel = camera.metresPerRow();

    if (view.showOrbits) {
        for (std::size_t i = 0; i < system.size(); ++i) {
            const auto* kepler =
                dynamic_cast<const omma::KeplerEphemeris*>(system.bodies()[i].get());
            if (kepler == nullptr) {
                continue;
            }
            const BodyStyle& style = styleFor(i);
            drawOrbit(canvas, camera, *kepler, t, style.colour.scaled(style.trailScale));
        }
    }

    // Collect, then sort by depth, then draw. Painter's algorithm: nearer
    // bodies drawn last so they occlude further ones. Sorting by GM would put
    // the Sun on top always, which is wrong the moment the camera is tilted and
    // Mercury passes in front of it.
    struct Visible {
        std::size_t index;
        omma::render::ScreenPoint point;
        double depth;
        int radius;
    };
    std::vector<Visible> visible;
    visible.reserve(system.size());

    for (std::size_t i = 0; i < system.size(); ++i) {
        const auto& body = *system.bodies()[i];
        const omma::Vec3 position = body.sample(t).position;
        const auto point = camera.project(position);
        if (!point.onScreen) {
            continue;
        }
        visible.push_back({i, point, camera.depthOf(position),
                           apparentRadiusPixels(body.meanRadius(), metresPerPixel)});
    }

    std::sort(visible.begin(), visible.end(),
              [](const Visible& a, const Visible& b) { return a.depth < b.depth; });

    // Glow first, all of it, so one body's corona cannot wash out another's
    // disc drawn earlier.
    for (const Visible& v : visible) {
        const BodyStyle& style = styleFor(v.index);
        if (style.glowScale > 0.0) {
            const int radius = std::max(3, static_cast<int>(
                static_cast<double>(v.radius) * style.glowScale));
            canvas.glow(v.point.x, v.point.y, radius, style.colour.scaled(0.55));
        }
    }

    for (const Visible& v : visible) {
        canvas.disc(v.point.x, v.point.y, v.radius, styleFor(v.index).colour);
    }

    // Labels last, and only where they fit.
    //
    // Zoomed out, the inner planets land within a couple of pixels of each
    // other and naive labelling produced "V.VMoonhy" — four names overwriting
    // one another into nonsense. Two rules fix it: never draw over an occupied
    // cell or a lit pixel, and keep labels apart from each other. The focused
    // body is placed first so it always wins the space.
    if (!view.showLabels) {
        return;
    }

    struct Label { int cellX; int cellY; };
    std::vector<Label> placed;

    auto tryLabel = [&](const Visible& v) {
        const std::string_view name = system.bodies()[v.index]->name();
        const int cellX = v.point.x + v.radius + 2;
        const int cellY = v.point.y / 2;      // two pixel rows per character cell

        for (const Label& other : placed) {
            if (other.cellY == cellY && std::abs(other.cellX - cellX) < 12) {
                return;
            }
        }
        if (!canvas.spanIsClear(cellX, cellY, static_cast<int>(name.size()))) {
            return;
        }
        canvas.text(cellX, cellY, name,
                    v.index == view.focusIndex ? Ink::BrightWhite : Ink::BrightBlack);
        placed.push_back({cellX, cellY});
    };

    for (const Visible& v : visible) {
        if (v.index == view.focusIndex) tryLabel(v);
    }
    for (const Visible& v : visible) {
        if (v.index != view.focusIndex) tryLabel(v);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HUD
// ─────────────────────────────────────────────────────────────────────────────

std::string formatDistance(double metres) {
    char buffer[32];
    if (metres < 1.0e7) {
        std::snprintf(buffer, sizeof(buffer), "%.0f km", metres / 1000.0);
    } else if (metres < 0.01 * kAu) {
        std::snprintf(buffer, sizeof(buffer), "%.3f Gm", metres / 1.0e9);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.4f au", metres / kAu);
    }
    return std::string{buffer};
}

void drawHud(Canvas& canvas, const SolarSystem& system, const omma::SimClock& clock,
             const Camera& camera, const ViewState& view, double framesPerSecond,
             bool clamped, const FrameTimings& timings) {
    const int h = canvas.height();
    const Epoch t = clock.now();
    const auto& focus = system[static_cast<BodyId>(view.focusIndex)];

    // Clear the strips the HUD occupies. Pixels showing through between the
    // letters turns a readout into noise.
    canvas.fill(0, 0, canvas.width(), 1);
    canvas.fill(0, h - 2, canvas.width(), 2);
    canvas.fill(0, 2, 27, 11);

    char line[256];
    std::snprintf(line, sizeof(line), " %s   %s ",
                  omma::versionBanner().data(), t.toString().c_str());
    canvas.text(1, 0, line, Ink::BrightWhite);

    std::snprintf(line, sizeof(line), "T+%.1f d  |  %.0f fps ",
                  omma::toSeconds(clock.elapsed()) / kSecondsPerDay, framesPerSecond);
    canvas.text(std::max(1, canvas.width() - static_cast<int>(std::string(line).size()) - 1),
                0, line, Ink::BrightBlack);

    const auto& warp = kWarpLadder[view.warpIndex];
    std::snprintf(line, sizeof(line),
                  " warp %s%-11s   scale %s   tilt %.0f deg   spin %.0f deg   focus %s",
                  view.paused ? "PAUSED " : "", warp.label,
                  formatDistance(camera.viewWidthMetres()).c_str(),
                  omma::toDegrees(camera.elevation()),
                  omma::toDegrees(camera.azimuth()),
                  focus.name().data());
    canvas.text(1, h - 2, line, view.paused ? Ink::BrightYellow : Ink::BrightGreen);

    if (clamped) {
        canvas.text(canvas.width() - 18, h - 2, "[falling behind]", Ink::BrightRed);
    }

    canvas.text(1, h - 1,
                " space pause  -/= warp  [/] zoom  1-5 presets  r/t tilt  z/x spin"
                "  tab focus  o orbits  ? help  q quit",
                Ink::BrightBlack);

    // ── focus panel ─────────────────────────────────────────────────────────
    const auto* kepler = dynamic_cast<const omma::KeplerEphemeris*>(&focus);
    int row = 2;
    canvas.text(1, row++, focus.name(), Ink::BrightWhite);
    if (kepler != nullptr) {
        const auto elements = kepler->elementsAt(t);
        const auto state = kepler->sampleRelativeToParent(t);

        const struct { const char* label; std::string value; } fields[] = {
            {"r",      formatDistance(state.position.norm())},
            {"v",      [&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.3f km/s",
                                           state.velocity.norm() / 1000.0);
                             return std::string{b}; }()},
            {"a",      formatDistance(elements.semiMajorAxis)},
            {"e",      [&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.5f", elements.eccentricity);
                             return std::string{b}; }()},
            {"i",      [&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.3f deg",
                                           omma::toDegrees(elements.inclination));
                             return std::string{b}; }()},
            {"peri",   formatDistance(periapsisRadius(elements))},
            {"apo",    formatDistance(apoapsisRadius(elements))},
            {"period", [&] {
                 const double days =
                     orbitalPeriod(elements, kepler->propagationGm()) / kSecondsPerDay;
                 char b[24];
                 if (days < 900.0) std::snprintf(b, sizeof(b), "%.2f d", days);
                 else              std::snprintf(b, sizeof(b), "%.2f y", days / 365.25);
                 return std::string{b}; }()},
        };
        for (const auto& [label, value] : fields) {
            std::snprintf(line, sizeof(line), "  %-6s %s", label, value.c_str());
            canvas.text(1, row++, line, Ink::White);
        }
    } else {
        canvas.text(1, row++, "  the frame origin", Ink::BrightBlack);
    }

    // ── frame timing overlay ────────────────────────────────────────────────
    if (view.showTimings) {
        const int x = std::max(0, canvas.width() - 30);
        canvas.fill(x, 2, 30, 9);
        canvas.text(x, 2, "frame budget 33.33 ms", Ink::BrightWhite);
        const struct { const char* name; double ms; } rows[] = {
            {"draw",    timings.drawMs},
            {"present", timings.presentMs},
            {"write",   timings.writeMs},
            {"sleep",   timings.sleepMs},
            {"total",   timings.totalMs},
        };
        int r = 3;
        for (const auto& [name, ms] : rows) {
            std::snprintf(line, sizeof(line), "  %-8s %6.2f ms", name, ms);
            canvas.text(x, r++, line, ms > 20.0 ? Ink::BrightRed : Ink::BrightBlack);
        }
        std::snprintf(line, sizeof(line), "  %-8s %6.1f KB",
                      "frame", static_cast<double>(timings.frameBytes) / 1024.0);
        canvas.text(x, r, line, Ink::BrightBlack);
    }

    // ── help overlay ────────────────────────────────────────────────────────
    if (view.showHelp) {
        static constexpr const char* kHelp[] = {
            "+---------------------------------------------+",
            "|  space    pause / resume                    |",
            "|  - =      time warp down / up               |",
            "|  [ ]      zoom out / in                     |",
            "|  1 - 5    zoom presets                      |",
            "|  r t      tilt the board down / up           |",
            "|  z x      spin the board left / right        |",
            "|  v        toggle top-down / dimetric         |",
            "|  w a s d  pan          c  re-centre         |",
            "|  tab / n  next body    p  previous body     |",
            "|  o l      orbit trails / labels             |",
            "|  f        frame timing breakdown            |",
            "|  ?        this panel   q / esc  quit        |",
            "+---------------------------------------------+",
        };
        const int boxWidth = 47;
        const int x = std::max(0, (canvas.width() - boxWidth) / 2);
        const int y = std::max(0, (canvas.height() - static_cast<int>(std::size(kHelp))) / 2);
        canvas.fill(x, y, boxWidth, static_cast<int>(std::size(kHelp)));
        for (std::size_t i = 0; i < std::size(kHelp); ++i) {
            canvas.text(x, y + static_cast<int>(i), kHelp[i], Ink::BrightWhite);
        }
    }
}

void render(Canvas& canvas, Camera& camera, const SolarSystem& system,
            const omma::SimClock& clock, const ViewState& view,
            double framesPerSecond, bool clamped, const FrameTimings& timings) {
    const Epoch t = clock.now();
    canvas.clear();
    camera.setCentre(system[static_cast<BodyId>(view.focusIndex)].sample(t).position
                     + view.panOffset);
    drawScene(canvas, camera, system, t, view);
    drawHud(canvas, system, clock, camera, view, framesPerSecond, clamped, timings);
}

// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────

void handleKey(int key, ViewState& view, Camera& camera, const SolarSystem& system) {
    const double panStep = camera.metresPerRow() * 12.0;
    constexpr double kTiltStep = 3.0 * omma::constants::kPi / 180.0;
    constexpr double kSpinStep = 6.0 * omma::constants::kPi / 180.0;

    switch (key) {
        case 'q': case 'Q': case 0x1b: case 0x03:
            view.running = false; break;
        case ' ':
            view.paused = !view.paused; break;

        case '=': case '+':
            if (view.warpIndex + 1 < kWarpLadder.size()) ++view.warpIndex; break;
        case '-': case '_':
            if (view.warpIndex > 0) --view.warpIndex; break;

        case ']': camera.zoomBy(1.0 / 1.35); break;
        case '[': camera.zoomBy(1.35); break;

        case '1': case '2': case '3': case '4': case '5':
            camera.frame(kZoomPresets[static_cast<std::size_t>(key - '1')].metresAcross);
            break;

        // Tilt and spin. This is the whole isometric feature, in six lines,
        // because the projection was the only thing that had to change.
        case 'r': case 'R': camera.tiltBy(-kTiltStep); break;
        case 't': case 'T': camera.tiltBy(+kTiltStep); break;
        case 'z': case 'Z': camera.spinBy(-kSpinStep); break;
        case 'x': case 'X': camera.spinBy(+kSpinStep); break;
        case 'v': case 'V':
            camera.setElevation(camera.elevation() > Camera::kDimetricElevation + 0.01
                                    ? Camera::kDimetricElevation
                                    : Camera::kTopDownElevation);
            break;

        case 'w': case 'W': view.panOffset.y += panStep; break;
        case 's': case 'S': view.panOffset.y -= panStep; break;
        case 'a': case 'A': view.panOffset.x -= panStep; break;
        case 'd': case 'D': view.panOffset.x += panStep; break;
        case 'c': case 'C': view.panOffset = omma::Vec3::zero(); break;

        case '\t': case 'n': case 'N':
            view.focusIndex = (view.focusIndex + 1) % system.size();
            view.panOffset = omma::Vec3::zero();
            break;
        case 'p': case 'P':
            view.focusIndex = (view.focusIndex + system.size() - 1) % system.size();
            view.panOffset = omma::Vec3::zero();
            break;

        case 'o': case 'O': view.showOrbits = !view.showOrbits; break;
        case 'l': case 'L': view.showLabels = !view.showLabels; break;
        case '?': case 'h': case 'H': view.showHelp = !view.showHelp; break;
        case 'f': case 'F': view.showTimings = !view.showTimings; break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Command line
//
// --snapshot exists so the renderer can be verified without a human watching
// it. A visual system that can only be checked by looking at it is a visual
// system nobody checks, and it rots quietly.
// ─────────────────────────────────────────────────────────────────────────────
struct Options {
    bool        snapshot{false};
    ColourDepth depth{ColourDepth::TrueColour};
    int         columns{0};
    int         rows{0};
    std::size_t zoomPreset{3};
    std::string focus{"Sun"};
    omma::CivilTime date{2026, 8, 6, 12, 0, 0.0};
    double      elevationDeg{30.0};
    double      azimuthDeg{0.0};
    bool        showHelpAndExit{false};
};

void printUsage() {
    std::puts(
        "omma-ascii - the solar system, in your terminal\n"
        "\n"
        "  (no arguments)      run interactively\n"
        "  --snapshot          render one frame to stdout and exit\n"
        "  --colour MODE       truecolour (default), ansi16, or ascii\n"
        "  --size WxH          override the terminal size, in character cells\n"
        "  --zoom N            zoom preset 1-5 (Earth-Moon .. everything)\n"
        "  --focus NAME        centre on a body, e.g. Earth\n"
        "  --date YYYY-MM-DD   start epoch\n"
        "  --tilt DEG          camera elevation; 90 is top-down, 30 is dimetric\n"
        "  --spin DEG          camera azimuth\n"
        "  --help              this text");
}

bool parseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string{argv[++i]} : std::string{};
        };

        if (arg == "--help" || arg == "-h") {
            out.showHelpAndExit = true;
        } else if (arg == "--snapshot") {
            out.snapshot = true;
        } else if (arg == "--colour" || arg == "--color") {
            const std::string mode = next();
            if (mode == "truecolour" || mode == "truecolor") out.depth = ColourDepth::TrueColour;
            else if (mode == "ansi16")                       out.depth = ColourDepth::Ansi16;
            else if (mode == "ascii" || mode == "none")      out.depth = ColourDepth::Ascii;
            else { std::fprintf(stderr, "--colour wants truecolour, ansi16 or ascii\n");
                   return false; }
        } else if (arg == "--no-colour" || arg == "--no-color") {
            out.depth = ColourDepth::Ascii;
        } else if (arg == "--size") {
            const std::string value = next();
            const auto x = value.find('x');
            if (x == std::string::npos) {
                std::fprintf(stderr, "--size wants WxH, e.g. 120x40\n");
                return false;
            }
            out.columns = std::atoi(value.substr(0, x).c_str());
            out.rows = std::atoi(value.substr(x + 1).c_str());
        } else if (arg == "--zoom") {
            const int n = std::atoi(next().c_str());
            if (n < 1 || n > static_cast<int>(kZoomPresets.size())) {
                std::fprintf(stderr, "--zoom wants 1..%zu\n", kZoomPresets.size());
                return false;
            }
            out.zoomPreset = static_cast<std::size_t>(n - 1);
        } else if (arg == "--focus") {
            out.focus = next();
        } else if (arg == "--tilt") {
            out.elevationDeg = std::atof(next().c_str());
        } else if (arg == "--spin") {
            out.azimuthDeg = std::atof(next().c_str());
        } else if (arg == "--date") {
            const std::string value = next();
            if (value.size() < 10) {
                std::fprintf(stderr, "--date wants YYYY-MM-DD\n");
                return false;
            }
            out.date.year = std::atoi(value.substr(0, 4).c_str());
            out.date.month = static_cast<unsigned>(std::atoi(value.substr(5, 2).c_str()));
            out.date.day = static_cast<unsigned>(std::atoi(value.substr(8, 2).c_str()));
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::size_t focusIndexFor(const SolarSystem& system, std::string_view name) {
    for (std::size_t i = 0; i < system.size(); ++i) {
        if (system.bodies()[i]->name() == name) {
            return i;
        }
    }
    return 0;
}

/// A Camera measured in half-block PIXELS, not character cells.
///
/// Two pixel rows per cell, and each pixel is square — so the unit aspect is
/// 1.0, not the 2.0 a character grid needs. This is the entire payoff of the
/// half-block trick, and also the one place it is easy to get wrong: pass 2.0
/// here and every orbit comes out an egg.
Camera makePixelCamera(const Canvas& canvas, const Options& options) {
    Camera camera{canvas.pixelWidth(), canvas.pixelHeight(), /*unitAspect=*/1.0};
    camera.setElevation(omma::toRadians(options.elevationDeg));
    camera.setAzimuth(omma::toRadians(options.azimuthDeg));
    camera.frame(kZoomPresets[options.zoomPreset].metresAcross);
    return camera;
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }
    if (options.showHelpAndExit) {
        printUsage();
        return 0;
    }

    const SolarSystem system = SolarSystem::standard();

    // ── snapshot: one frame, no terminal takeover ───────────────────────────
    if (options.snapshot) {
        Canvas canvas{options.columns > 0 ? options.columns : 110,
                      options.rows > 0 ? options.rows : 34};
        Camera camera = makePixelCamera(canvas, options);
        omma::SimClock snapshotClock{std::chrono::seconds{1}, Epoch::fromCivil(options.date)};

        ViewState view{};
        view.focusIndex = focusIndexFor(system, options.focus);

        render(canvas, camera, system, snapshotClock, view, 30.0, false, FrameTimings{});

        // Colour is honoured as asked even when piped: a snapshot is often
        // captured deliberately, and forcing it to plain text would make
        // --colour a lie.
        if (options.depth != ColourDepth::Ascii) {
            con::enableAnsi();
        }
        std::string frame;
        canvas.present(frame, options.depth, /*homeCursor=*/false);
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    // ── interactive ─────────────────────────────────────────────────────────
    con::InteractiveSession session;
    if (!session.ok()) {
        std::fprintf(stderr,
                     "omma-ascii needs an interactive terminal.\n"
                     "Run it directly rather than through a pipe, or use\n"
                     "  omma-ascii --snapshot\n"
                     "for a single frame you can redirect.\n");
        return 1;
    }

    omma::SimClock clock{std::chrono::seconds{1}, Epoch::fromCivil(options.date)};

    // maxStepsPerFrame is deliberately enormous. That clamp bounds INTEGRATION
    // work per frame, and nothing here is being integrated: every body is
    // analytic, so advancing the tick count is one integer add whether it is by
    // one or by a billion. When spacecraft arrive this comes back down to what
    // the RK4 kernel can actually finish inside a frame.
    omma::StepPacer pacer{std::chrono::seconds{1}, 1'000'000'000'000LL};

    auto size = con::terminalSize();
    Canvas canvas{size.columns, size.rows};
    Camera camera = makePixelCamera(canvas, options);

    ViewState view{};
    view.focusIndex = focusIndexFor(system, options.focus);

    std::string frameBuffer;
    frameBuffer.reserve(static_cast<std::size_t>(size.columns * size.rows) * 24);

    using Clock = std::chrono::steady_clock;
    auto lastFrame = Clock::now();
    double smoothedFps = 30.0;
    bool clamped = false;
    FrameTimings timings{};

    while (view.running) {
        // Drain the key queue rather than reading one per frame; a held key
        // otherwise builds a backlog that keeps acting long after release.
        while (const int key = con::pollKey()) {
            handleKey(key, view, camera, system);
        }

        const auto frameStart = Clock::now();
        const double realDt = std::chrono::duration<double>(frameStart - lastFrame).count();
        lastFrame = frameStart;
        if (realDt > 0.0) {
            smoothedFps = 0.9 * smoothedFps + 0.1 / realDt;
        }

        const double warp = view.paused ? 0.0 : kWarpLadder[view.warpIndex].factor;
        clock.step(pacer.stepsForFrame(realDt, warp));
        clamped = pacer.lastFrameWasClamped();

        const auto current = con::terminalSize();
        if (current.columns != size.columns || current.rows != size.rows) {
            size = current;
            canvas.resize(size.columns, size.rows);
            camera.setViewport(canvas.pixelWidth(), canvas.pixelHeight());
            std::fputs(con::clearToEnd().data(), stdout);
        }

        const auto drawStart = Clock::now();
        render(canvas, camera, system, clock, view, smoothedFps, clamped, timings);
        const auto presentStart = Clock::now();
        canvas.present(frameBuffer, options.depth, /*homeCursor=*/true);
        const auto writeStart = Clock::now();
        std::fwrite(frameBuffer.data(), 1, frameBuffer.size(), stdout);
        std::fflush(stdout);
        const auto writeEnd = Clock::now();

        // con::sleepFor, not std::this_thread::sleep_for. See the comment on
        // its declaration: Windows' 15.625 ms timer tick rounds a portable
        // sleep up hard enough to turn a 30 fps target into 21, and the
        // renderer gets blamed for it.
        constexpr auto kFrameBudget = std::chrono::duration<double>{1.0 / 30.0};
        const auto spent = std::chrono::duration<double>{writeEnd - frameStart};
        if (spent < kFrameBudget) {
            con::sleepFor(
                std::chrono::duration_cast<std::chrono::nanoseconds>(kFrameBudget - spent));
        }

        const auto frameEnd = Clock::now();
        const auto elapsedMs = [](auto from, auto to) {
            return std::chrono::duration<double, std::milli>{to - from}.count();
        };
        timings.drawMs = elapsedMs(drawStart, presentStart);
        timings.presentMs = elapsedMs(presentStart, writeStart);
        timings.writeMs = elapsedMs(writeStart, writeEnd);
        timings.sleepMs = elapsedMs(writeEnd, frameEnd);
        timings.totalMs = elapsedMs(frameStart, frameEnd);
        timings.frameBytes = frameBuffer.size();
    }

    return 0;
}
