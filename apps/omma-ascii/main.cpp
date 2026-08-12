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
#include "sim/world.hpp"
#include "render/camera.hpp"
#include "render/canvas.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace con = omma::console;
using omma::app::BodyStyle;
using omma::app::styleFor;
using omma::BodyId;
using omma::Epoch;
using omma::LaunchRequest;
using omma::SolarSystem;
using omma::Spacecraft;
using omma::ThrustCommand;
using omma::World;
using omma::constants::kAu;
using omma::constants::kSecondsPerDay;
using omma::render::Camera;
using omma::render::Canvas;
using omma::render::BlockStyle;
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

constexpr std::array<ZoomPreset, 6> kZoomPresets{{
    {3.0e7,  "low orbit"},      // a 400 km orbit fills about a quarter of the view
    {1.2e9,  "Earth-Moon"},
    {3.6e11, "inner system"},
    {1.7e12, "to Jupiter"},
    {9.5e12, "to Neptune"},
    {1.6e13, "everything"},
}};

/// Framing used when something is launched, matching preset 1.
constexpr double kLowOrbitSpan = 3.0e7;

// ─────────────────────────────────────────────────────────────────────────────
// Spacecraft presentation.
//
// Deliberately the brightest thing on screen after the Sun. The planets are
// scenery; the spacecraft is the subject, and at one or two pixels across it
// needs every bit of contrast it can get.
// ─────────────────────────────────────────────────────────────────────────────
constexpr Rgb kCraftColour{120, 255, 170};      ///< a coasting craft
constexpr Rgb kCraftBurning{255, 190, 90};      ///< engine lit
constexpr Rgb kCraftDead{110, 90, 90};          ///< wreckage
constexpr Rgb kCraftOrbit{40, 150, 110};        ///< its predicted path
constexpr Rgb kCraftOrbitFocused{90, 240, 180};

/// Warp is no longer free once anything is being integrated.
///
/// With every body analytic, advancing the clock a billion ticks cost one integer
/// addition. A spacecraft changes that: each tick is four RK4 stages over eleven
/// gravity sources, so the steps-per-frame cap has to come down to what the kernel
/// can actually finish inside a frame. This is the promise the old comment made,
/// now being paid.
///
/// The budget is per frame, shared across craft, and the HUD says
/// "[falling behind]" when it binds -- which is the honest thing to show. Simulated
/// time falling behind the wall clock is correct behaviour; pretending otherwise
/// is the spiral of death.
constexpr std::int64_t kIntegrationStepBudget = 24'000;

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

/// Set when something has just been launched and the camera should zoom to it.
/// A free variable rather than a ViewState field because it is a one-shot request
/// from the input handler to the frame loop, not part of the view's state.
bool camera_frame_hint = false;

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
void drawEllipse(Canvas& canvas, const Camera& camera,
                 const omma::OrbitalElements& elements,
                 const omma::Vec3& parentOrigin, Rgb colour) {
    const double a = elements.semiMajorAxis;
    const double e = elements.eccentricity;
    if (!(a > 0.0) || e >= 1.0) {
        return;
    }

    // Cull orbits that carry no information at this zoom.
    //
    // Zoomed in on the Earth-Moon system, Earth's own heliocentric orbit is 125
    // times wider than the view, so it renders as a straight diagonal line
    // across the screen: correct, and pure noise. Zoomed out to Neptune, the
    // Moon's orbit is a hundredth of a pixel.
    //
    // Note the generous upper bound. An orbit a few times wider than the view
    // still shows as a curving arc that tells you where the next planet out is,
    // and those arcs are worth keeping -- so this culls the useless extremes
    // rather than everything that does not fit.
    const double viewSpan = camera.viewWidthMetres();
    if (a > viewSpan * 25.0 || a < camera.metresPerRow() * 1.5) {
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
        // Draw whenever BOTH endpoints are representable, regardless of whether
        // either is on screen, and let Canvas::line clip the segment.
        //
        // The previous condition was `point.onScreen || previous.onScreen`,
        // which silently dropped every chord whose two endpoints straddle the
        // viewport -- so any orbit larger than the view rendered as a handful of
        // dashes near the edges. Clipping is the renderer's job, not the
        // caller's; asking "is this visible" at the call site gets it wrong for
        // exactly the segments that matter most.
        if (havePrevious && point.valid && previous.valid) {
            canvas.line(previous.x, previous.y, point.x, point.y, colour);
        }
        previous = point;
        havePrevious = true;
    }
}

/// A celestial body's orbit.
void drawOrbit(Canvas& canvas, const Camera& camera,
               const omma::KeplerEphemeris& body, Epoch t, Rgb colour) {
    drawEllipse(canvas, camera, body.elementsAt(t),
                body.parent() != nullptr ? body.parent()->sample(t).position
                                         : omma::Vec3::zero(),
                colour);
}

/// A spacecraft's PREDICTED path: the osculating ellipse it is on right now.
///
/// "Osculating" means kissing -- the two-body orbit that matches the craft's
/// current position and velocity exactly. It is where the craft would go if
/// nothing else ever touched it, so it shifts continuously under perturbation and
/// jumps the instant the engine lights. Watching it deform IS the feedback that
/// makes flying by hand possible: you point the nose, hold the burn, and see the
/// far side of the orbit climb.
void drawPredictedPath(Canvas& canvas, const Camera& camera, const World& world,
                       const Spacecraft& craft, bool focused) {
    const auto& central = *world.system().bodies()[craft.centralBodyIndex];
    drawEllipse(canvas, camera, world.elementsOf(craft),
                central.sample(world.now()).position,
                focused ? kCraftOrbitFocused : kCraftOrbit);
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

void drawScene(Canvas& canvas, Camera& camera, const World& world,
               const ViewState& view) {
    const SolarSystem& system = world.system();
    const Epoch t = world.now();
    const double metresPerPixel = camera.metresPerRow();

    if (view.showOrbits) {
        // Reference orbits first, the focused one last, so the bright line is
        // never crossed out by a dim one drawn after it.
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < system.size(); ++i) {
                const bool focused = i == view.focusIndex;
                if (focused != (pass == 1)) {
                    continue;
                }
                const auto* kepler =
                    dynamic_cast<const omma::KeplerEphemeris*>(system.bodies()[i].get());
                if (kepler == nullptr) {
                    continue;
                }
                drawOrbit(canvas, camera, *kepler, t,
                          omma::app::orbitColourFor(i, focused));
            }
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

    // Bodies, shaded as lit spheres.
    //
    // The light direction is worked out in SCREEN space, by projecting the Sun and
    // the body and taking the difference. That is a cheap trick with a real
    // limitation: it captures where the Sun is left-or-right and up-or-down of the
    // body, but not how far in front or behind, so the z component is supplied as
    // a constant lean toward the viewer. A physically exact version would rotate
    // the world-space Sun direction into the camera basis; this looks
    // indistinguishable at these sizes and costs two projections instead of a
    // matrix.
    const auto sunScreen = camera.project(system[BodyId::Sun].sample(t).position);

    for (const Visible& v : visible) {
        const Rgb colour = styleFor(v.index).colour;

        if (v.index == static_cast<std::size_t>(BodyId::Sun) || v.radius <= 1) {
            // The Sun is the light source, so it has no dark side to show.
            canvas.disc(v.point.x, v.point.y, v.radius, colour);
            continue;
        }

        double lx = static_cast<double>(sunScreen.x - v.point.x);
        double ly = static_cast<double>(sunScreen.y - v.point.y);
        if (std::abs(lx) < 1e-9 && std::abs(ly) < 1e-9) {
            lx = 1.0;   // degenerate: Sun exactly behind the body on screen
        }
        canvas.shadedDisc(v.point.x, v.point.y, v.radius, colour, lx, ly, 0.55);
    }

    // ── spacecraft ──────────────────────────────────────────────────────────
    // After the planets, so a satellite is never hidden behind the body it
    // orbits, and drawn with a glow while burning so a lit engine is visible even
    // when the craft itself is one pixel.
    const auto& fleet = world.spacecraft();
    for (std::size_t i = 0; i < fleet.size(); ++i) {
        const Spacecraft& craft = fleet[i];
        const bool focused = view.focusIndex == system.size() + i;

        if (view.showOrbits && craft.isAlive()) {
            drawPredictedPath(canvas, camera, world, craft, focused);
        }

        const auto point = camera.project(craft.state.position);
        if (!point.onScreen) {
            continue;
        }

        const bool burning = craft.thrust.active && craft.thrust.throttle > 0.0;
        const Rgb colour = !craft.isAlive() ? kCraftDead
                         : burning          ? kCraftBurning
                                            : kCraftColour;
        if (burning) {
            canvas.glow(point.x, point.y, 4, colour.scaled(0.45));
        }
        canvas.disc(point.x, point.y, focused ? 1 : 0, colour);

        if (view.showLabels) {
            const int cellY = point.y / 2;
            if (canvas.spanIsClear(point.x + 2, cellY,
                                   static_cast<int>(craft.name.size()))) {
                canvas.text(point.x + 2, cellY, craft.name,
                            focused ? Ink::BrightWhite : Ink::BrightGreen);
            }
        }
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
        // BrightBlack is a legible slate now, not the almost-invisible grey the
        // ANSI palette calls by that name.
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
// Focus
//
// One index over bodies THEN spacecraft, so `tab` walks the whole scene and the
// camera-follow code does not care which kind of thing it is looking at. Adding a
// second "focusCraftIndex" field would have meant every read site asking "which
// one is set", which is the shape that grows inconsistent states.
// ─────────────────────────────────────────────────────────────────────────────

std::size_t focusTargetCount(const World& world) {
    return world.system().size() + world.spacecraft().size();
}

omma::Vec3 focusPosition(const World& world, const ViewState& view) {
    const std::size_t bodies = world.system().size();
    if (view.focusIndex < bodies) {
        return world.system().bodies()[view.focusIndex]->sample(world.now()).position;
    }
    const std::size_t craftIndex = view.focusIndex - bodies;
    if (craftIndex < world.spacecraft().size()) {
        return world.spacecraft()[craftIndex].state.position;
    }
    return omma::Vec3::zero();
}

const Spacecraft* focusedCraft(const World& world, const ViewState& view) {
    const std::size_t bodies = world.system().size();
    if (view.focusIndex < bodies) {
        return nullptr;
    }
    const std::size_t craftIndex = view.focusIndex - bodies;
    return craftIndex < world.spacecraft().size() ? &world.spacecraft()[craftIndex]
                                                  : nullptr;
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

void drawHud(Canvas& canvas, const World& world, const Camera& camera,
             const ViewState& view, double framesPerSecond, bool clamped,
             const FrameTimings& timings) {
    const SolarSystem& system = world.system();
    const omma::SimClock& clock = world.clock();
    const int h = canvas.height();
    const Epoch t = clock.now();

    const Spacecraft* craft = focusedCraft(world, view);
    const std::size_t bodyIndex = std::min(view.focusIndex, system.size() - 1);
    const auto& focus = *system.bodies()[bodyIndex];
    const auto* kepler = craft != nullptr
                             ? nullptr
                             : dynamic_cast<const omma::KeplerEphemeris*>(&focus);

    // Clear the strips the HUD occupies. Pixels showing through between the
    // letters turns a readout into noise.
    canvas.fill(0, 0, canvas.width(), 1);
    canvas.fill(0, h - 2, canvas.width(), 2);
    // Only as many rows as the panel actually writes. Blanking a fixed 11 rows
    // took a visible rectangular bite out of the scene even when the focused
    // body was the Sun and the panel used two of them.
    canvas.fill(0, 2, 30, (kepler != nullptr || craft != nullptr) ? 12 : 2);

    char line[256];
    std::snprintf(line, sizeof(line), " %s   %s ",
                  omma::versionBanner().data(), t.toString().c_str());
    canvas.text(1, 0, line, Ink::BrightWhite);

    std::snprintf(line, sizeof(line), "T+%.1f d  |  %.0f fps ",
                  omma::toSeconds(clock.elapsed()) / kSecondsPerDay, framesPerSecond);
    canvas.text(std::max(1, canvas.width() - static_cast<int>(std::string(line).size()) - 1),
                0, line, Ink::BrightBlack);   // now a readable slate, not near-black

    const auto& warp = kWarpLadder[view.warpIndex];
    std::snprintf(line, sizeof(line),
                  " warp %s%-11s   scale %s   tilt %.0f deg   spin %.0f deg   focus %s",
                  view.paused ? "PAUSED " : "", warp.label,
                  formatDistance(camera.viewWidthMetres()).c_str(),
                  omma::toDegrees(camera.elevation()),
                  omma::toDegrees(camera.azimuth()),
                  craft != nullptr ? craft->name.c_str() : focus.name().data());
    canvas.text(1, h - 2, line, view.paused ? Ink::BrightYellow : Ink::BrightGreen);

    if (clamped) {
        canvas.text(canvas.width() - 18, h - 2, "[falling behind]", Ink::BrightRed);
    }

    std::snprintf(line, sizeof(line),
                  " space pause  -/= warp  [/] zoom  1-6 presets  r/t tilt  z/x spin"
                  "  tab focus  L launch  ,/. burn  %zu craft  ? help  q quit",
                  world.spacecraft().size());
    canvas.text(1, h - 1, line, Ink::White);

    // ── focus panel ─────────────────────────────────────────────────────────
    int row = 2;

    if (craft != nullptr) {
        canvas.text(1, row++, craft->name, Ink::BrightWhite);
        const auto elements = world.elementsOf(*craft);
        const auto& central = *system.bodies()[craft->centralBodyIndex];
        const double gm = central.gravitationalParameter();

        const struct { const char* label; std::string value; } fields[] = {
            {"orbits", std::string{central.name()}},
            {"alt",    formatDistance(world.altitudeOf(*craft))},
            {"peri",   formatDistance(periapsisRadius(elements) - central.meanRadius())},
            {"apo",    formatDistance(apoapsisRadius(elements) - central.meanRadius())},
            {"e",      [&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.5f", elements.eccentricity);
                             return std::string{b}; }()},
            {"i",      [&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.2f deg",
                                           omma::toDegrees(elements.inclination));
                             return std::string{b}; }()},
            {"period", [&] { const double m = orbitalPeriod(elements, gm) / 60.0;
                             char b[24];
                             std::snprintf(b, sizeof(b), "%.1f min", m);
                             return std::string{b}; }()},
            {"prop",   [&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.1f kg", craft->propellantKg);
                             return std::string{b}; }()},
            {"dv left",[&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.0f m/s",
                                           craft->remainingDeltaVMps());
                             return std::string{b}; }()},
            {"dv used",[&] { char b[24];
                             std::snprintf(b, sizeof(b), "%.1f m/s",
                                           craft->deltaVSpentMps);
                             return std::string{b}; }()},
        };
        for (const auto& [label, value] : fields) {
            std::snprintf(line, sizeof(line), "  %-7s", label);
            canvas.text(1, row, line, Ink::BrightBlack);
            canvas.text(10, row, value, Ink::BrightCyan);
            ++row;
        }
        if (craft->thrust.active) {
            canvas.text(1, row, "  BURNING", Ink::BrightYellow);
        } else if (!craft->isAlive()) {
            canvas.text(1, row, "  DESTROYED", Ink::BrightRed);
        }
        return;
    }

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
            // Label dim, value bright: the numbers are what you are reading, and
            // a uniform grey makes you hunt for them.
            std::snprintf(line, sizeof(line), "  %-6s", label);
            canvas.text(1, row, line, Ink::BrightBlack);
            canvas.text(9, row, value, Ink::BrightCyan);
            ++row;
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
            "|  1 - 6    zoom presets (1 = low orbit)      |",
            "|  r t      tilt the board down / up           |",
            "|  z x      spin the board left / right        |",
            "|  v        toggle top-down / dimetric         |",
            "|  w a s d  pan          c  re-centre         |",
            "|  tab / n  next body    p  previous body     |",
            "|  L        launch a satellite here           |",
            "|  . ,      burn prograde / retrograde        |",
            "|  ' ;      burn normal / anti-normal         |",
            "|  k        cut the engine                    |",
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

void render(Canvas& canvas, Camera& camera, const World& world,
            const ViewState& view, double framesPerSecond, bool clamped,
            const FrameTimings& timings) {
    canvas.clear();
    // Deep space rather than pure black: a hint of blue reads as sky instead of
    // as a hole, and gives the dimmest orbit line something to sit against.
    for (int py = 0; py < canvas.pixelHeight(); ++py) {
        for (int px = 0; px < canvas.pixelWidth(); ++px) {
            canvas.setPixel(px, py, omma::app::kSpace);
        }
    }
    camera.setCentre(focusPosition(world, view) + view.panOffset);
    drawScene(canvas, camera, world, view);
    drawHud(canvas, world, camera, view, framesPerSecond, clamped, timings);
}

// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────

/// Put a satellite in low orbit around whichever body is currently in view.
///
/// A 400 km circular orbit, inclined 51.6 degrees -- the ISS inclination, chosen
/// because it is the one that reads clearly on a tilted display: an equatorial
/// orbit at 30 degrees of camera tilt looks like a flat line, which was the whole
/// argument for having tilt in the first place.
///
/// If the focus is a spacecraft, or something without a surface to orbit, it falls
/// back to Earth. Refusing to launch because the camera happens to be pointed at
/// the Sun would be technically defensible and annoying.
LaunchRequest makeLaunchRequest(BodyId around, int index) {
    LaunchRequest request{};
    char name[24];
    std::snprintf(name, sizeof(name), "OMMA-%d", index);
    request.name = name;
    request.aroundBody = around;
    request.altitudeMetres = 400.0e3;
    // 51.6 degrees, the ISS inclination, chosen because it reads clearly on a
    // tilted display: an equatorial orbit seen at 30 degrees of camera tilt looks
    // like a flat line, which was the whole argument for having tilt.
    request.inclinationRadians = omma::toRadians(51.6);
    // Spread a fleet around the orbit and across planes so it does not stack into
    // one pixel.
    request.meanAnomalyRadians = omma::toRadians(37.0 * static_cast<double>(index));
    request.longitudeOfAscendingNodeRadians =
        omma::toRadians(23.0 * static_cast<double>(index));
    request.propellantKg = 80.0;
    request.maxThrustNewtons = 400.0;   // ~1.4 m/s^2: burns take seconds, not hours
    return request;
}

void launchFromFocus(World& world, ViewState& view, int index) {
    const std::size_t bodies = world.system().size();
    std::size_t around = view.focusIndex < bodies ? view.focusIndex
                                                  : static_cast<std::size_t>(BodyId::Earth);
    if (around == static_cast<std::size_t>(BodyId::Sun)) {
        around = static_cast<std::size_t>(BodyId::Earth);
    }

    const auto id = world.launch(makeLaunchRequest(static_cast<BodyId>(around), index));
    if (id.isValid()) {
        // Follow what you just launched, and frame it. Launching something and
        // then having to hunt for it is a bad first five seconds.
        view.focusIndex = bodies + world.spacecraft().size() - 1;
        view.panOffset = omma::Vec3::zero();
        camera_frame_hint = true;
    }
}

/// Fire the focused craft's engine for a fixed slice of delta-v.
///
/// Ten metres per second per press: small enough to steer with, big enough to see
/// the predicted path move. Ignored when the focus is not a spacecraft, rather
/// than beeping about it.
void burnFocused(World& world, const ViewState& view, ThrustCommand::Frame frame) {
    const Spacecraft* craft = focusedCraft(world, view);
    if (craft == nullptr || !craft->isAlive()) {
        return;
    }
    world.commandDeltaV(craft->id, frame, 10.0);
}

void handleKey(int key, ViewState& view, Camera& camera, World& world) {
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

        case '1': case '2': case '3': case '4': case '5': case '6':
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

        case '\t': case 'n': case 'N': {
            const std::size_t count = focusTargetCount(world);
            view.focusIndex = (view.focusIndex + 1) % count;
            view.panOffset = omma::Vec3::zero();
            break;
        }
        case 'p': case 'P': {
            const std::size_t count = focusTargetCount(world);
            view.focusIndex = (view.focusIndex + count - 1) % count;
            view.panOffset = omma::Vec3::zero();
            break;
        }

        // ── flying things ───────────────────────────────────────────────────
        // Uppercase L launches; lowercase l toggles labels. Case-sensitive
        // bindings are worth avoiding in general, but "launch" deserves to be the
        // shifted one: it is the only key here that permanently changes the world,
        // and a stray keypress should not put a satellite in orbit.
        case 'L':
            launchFromFocus(world, view,
                            static_cast<int>(world.spacecraft().size()) + 1);
            break;
        case '.': burnFocused(world, view, ThrustCommand::Frame::Prograde); break;
        case ',': burnFocused(world, view, ThrustCommand::Frame::Retrograde); break;
        case '\'': burnFocused(world, view, ThrustCommand::Frame::Normal); break;
        case ';': burnFocused(world, view, ThrustCommand::Frame::AntiNormal); break;
        case 'k': case 'K':
            if (const Spacecraft* c = focusedCraft(world, view); c != nullptr) {
                world.cancelBurn(c->id);
            }
            break;

        case 'o': case 'O': view.showOrbits = !view.showOrbits; break;
        case 'l': view.showLabels = !view.showLabels; break;
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
    bool        depthForced{false};
    BlockStyle  blocks{BlockStyle::HalfBlocks};
    bool        blocksForced{false};
    int         columns{0};
    int         rows{0};
    std::size_t zoomPreset{4};   // "to Neptune"; see kZoomPresets
    std::string focus{"Sun"};
    omma::CivilTime date{2026, 8, 6, 12, 0, 0.0};
    double      elevationDeg{30.0};
    double      azimuthDeg{0.0};
    bool        showHelpAndExit{false};

    // ── record mode ─────────────────────────────────────────────────────────
    // Runs the LIVE loop headlessly, with a synthetic frame clock and a scripted
    // key stream, writing every frame's byte stream to disk.
    //
    // This exists because --snapshot bypasses the game loop entirely. Every
    // temporal defect — flicker, stale pixels, tearing, frame pacing — lives in
    // code that no test had ever executed. Replacing the two nondeterministic
    // inputs (the wall clock and the keyboard) with scripted ones makes the whole
    // loop reproducible and assertable, which is software-in-the-loop applied to
    // a renderer.
    int         recordFrames{0};
    std::string recordDir{"frames"};
    double      frameDeltaSeconds{1.0 / 30.0};
    std::string keys;             ///< one character per frame; '.' means none
    bool        startPaused{false};

    /// Pre-launch this many satellites before the first frame.
    ///
    /// Exists so the spacecraft rendering path is reachable from --snapshot, which
    /// takes no keystrokes. A feature that only the interactive binary can produce
    /// is a feature the QA harness cannot check.
    int         preLaunch{0};
};

void printUsage() {
    std::puts(
        "omma-ascii - the solar system, in your terminal\n"
        "\n"
        "  (no arguments)      run interactively\n"
        "  --snapshot          render one frame to stdout and exit\n"
        "  --colour MODE       truecolour (default), ansi16, or ascii\n"
        "  --blocks MODE       half (default; needs a UTF-8 console) or full\n"
        "  --size WxH          override the terminal size, in character cells\n"
        "  --zoom N            zoom preset 1-6 (low orbit .. everything)\n"
        "  --focus NAME        centre on a body, e.g. Earth\n"
        "  --date YYYY-MM-DD   start epoch\n"
        "  --tilt DEG          camera elevation; 90 is top-down, 30 is dimetric\n"
        "  --spin DEG          camera azimuth\n"
        "\n"
        "  --record N          run the live loop headlessly for N frames and\n"
        "                      write each frame's bytes to --record-dir\n"
        "  --record-dir DIR    where to write them (default: frames)\n"
        "  --frame-delta MS    synthetic wall-clock time per frame (default 33.3)\n"
        "  --keys STRING       one key per frame; '.' means no key that frame\n"
        "  --paused            start paused\n"
        "  --launch N          put N satellites in low Earth orbit first\n"
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
            out.depthForced = true;
        } else if (arg == "--no-colour" || arg == "--no-color") {
            out.depth = ColourDepth::Ascii;
            out.depthForced = true;
        } else if (arg == "--blocks") {
            const std::string mode = next();
            if (mode == "half")      out.blocks = BlockStyle::HalfBlocks;
            else if (mode == "full") out.blocks = BlockStyle::FullCells;
            else { std::fprintf(stderr, "--blocks wants half or full\n"); return false; }
            out.blocksForced = true;
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
        } else if (arg == "--record") {
            out.recordFrames = std::atoi(next().c_str());
            if (out.recordFrames <= 0) {
                std::fprintf(stderr, "--record wants a positive frame count\n");
                return false;
            }
        } else if (arg == "--record-dir") {
            out.recordDir = next();
        } else if (arg == "--frame-delta") {
            const double ms = std::atof(next().c_str());
            if (!(ms > 0.0)) {
                std::fprintf(stderr, "--frame-delta wants milliseconds > 0\n");
                return false;
            }
            out.frameDeltaSeconds = ms / 1000.0;
        } else if (arg == "--keys") {
            out.keys = next();
        } else if (arg == "--paused") {
            out.startPaused = true;
        } else if (arg == "--launch") {
            out.preLaunch = std::atoi(next().c_str());
            if (out.preLaunch < 0) {
                std::fprintf(stderr, "--launch wants a count >= 0\n");
                return false;
            }
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

/// Run the live loop headlessly and write every frame to disk.
///
/// This is the SAME loop the interactive path runs — same clock, same pacer, same
/// render(), same present() with homeCursor on, so the cursor-home and
/// erase-to-end-of-line escapes are exercised. The only substitutions are the two
/// nondeterministic inputs:
///
///     steady_clock::now()  ->  a fixed synthetic delta
///     con::pollKey()       ->  one scripted character per frame
///
/// Which is the whole trick. Replace a system's sensors with scripted ones and
/// capture its actuator, and a previously untestable feedback loop becomes a pure
/// function you can assert on. The frame rate reported to the HUD is pinned to
/// the synthetic rate rather than exponentially smoothed, so a paused recording
/// is byte-identical frame to frame — which turns "does it flicker" into a
/// one-line check.
int runRecording(const Options& options) {
    const int columns = options.columns > 0 ? options.columns : 120;
    const int rows = options.rows > 0 ? options.rows : 36;

    Canvas canvas{columns, rows};
    Camera camera = makePixelCamera(canvas, options);
    World world{SolarSystem::standard(), std::chrono::seconds{1},
                Epoch::fromCivil(options.date)};
    omma::StepPacer pacer{std::chrono::seconds{1}, 1'000'000'000'000LL};

    ViewState view{};
    view.focusIndex = focusIndexFor(world.system(), options.focus);
    view.paused = options.startPaused;
    for (int i = 0; i < options.preLaunch; ++i) {
        world.launch(makeLaunchRequest(BodyId::Earth, i + 1));
    }

    std::error_code error;
    std::filesystem::create_directories(options.recordDir, error);
    if (error) {
        std::fprintf(stderr, "could not create %s: %s\n",
                     options.recordDir.c_str(), error.message().c_str());
        return 1;
    }

    const double reportedFps = 1.0 / options.frameDeltaSeconds;
    std::string frame;

    for (int i = 0; i < options.recordFrames; ++i) {
        // '.' means "no key" in a script, which now collides with the prograde
        // burn binding. Scripts use '@' for prograde instead; the interactive
        // binding stays '.' because that is where a thumb expects it.
        if (static_cast<std::size_t>(i) < options.keys.size()) {
            const char scripted = options.keys[static_cast<std::size_t>(i)];
            if (scripted != '.') {
                // '@' is the script's spelling of the prograde-burn key, because
                // '.' is already spoken for as "no key this frame". Translating
                // after the no-key test, not before it -- doing it the other way
                // round silently swallowed every scripted burn.
                const int key = (scripted == '@') ? '.' : scripted;
                handleKey(key, view, camera, world);
                if (!view.running) {
                    break;
                }
            }
        }
        // Same one-shot framing the interactive loop does. Without it a scripted
        // launch records 45 frames of an invisible satellite, which is exactly what
        // happened the first time.
        if (camera_frame_hint) {
            camera.frame(kLowOrbitSpan);
            camera_frame_hint = false;
        }

        pacer.setMaxStepsPerFrame(world.spacecraft().empty()
                                      ? 1'000'000'000'000LL
                                      : kIntegrationStepBudget);
        const double warp = view.paused ? 0.0 : kWarpLadder[view.warpIndex].factor;
        world.step(pacer.stepsForFrame(options.frameDeltaSeconds, warp));

        // Timings are deliberately zeroed rather than measured: a recording must
        // not depend on how fast the machine writing it happens to be.
        render(canvas, camera, world, view, reportedFps,
               pacer.lastFrameWasClamped(), FrameTimings{});
        canvas.present(frame, options.depth, options.blocks, /*homeCursor=*/true);

        char name[64];
        std::snprintf(name, sizeof(name), "frame_%05d.bin", i);
        const std::filesystem::path path =
            std::filesystem::path{options.recordDir} / name;

        // Binary, so nothing translates a newline on the way to disk. A recording
        // that differs from what the terminal would have received is not a
        // recording of anything.
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) {
            std::fprintf(stderr, "could not write %s\n", path.string().c_str());
            return 1;
        }
        out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
    }

    std::printf("%d frames -> %s  (%dx%d cells, %.2f ms/frame synthetic)\n",
                options.recordFrames, options.recordDir.c_str(),
                columns, rows, options.frameDeltaSeconds * 1000.0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Before anything is written. A renderer that controls the cursor has to
    // control the bytes; see the declaration for what text mode does to them.
    con::useBinaryStdout();

    Options options{};
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }
    if (options.showHelpAndExit) {
        printUsage();
        return 0;
    }

    // ── record: the live loop, headless and deterministic ───────────────────
    if (options.recordFrames > 0) {
        return runRecording(options);
    }

    // ── snapshot: one frame, no terminal takeover ───────────────────────────
    if (options.snapshot) {
        Canvas canvas{options.columns > 0 ? options.columns : 110,
                      options.rows > 0 ? options.rows : 34};
        Camera camera = makePixelCamera(canvas, options);
        World world{SolarSystem::standard(), std::chrono::seconds{1},
                    Epoch::fromCivil(options.date)};

        ViewState view{};
        view.focusIndex = focusIndexFor(world.system(), options.focus);
        for (int i = 0; i < options.preLaunch; ++i) {
            world.launch(makeLaunchRequest(BodyId::Earth, i + 1));
        }

        render(canvas, camera, world, view, 30.0, false, FrameTimings{});

        // Colour is honoured as asked even when piped: a snapshot is often
        // captured deliberately, and forcing it to plain text would make
        // --colour a lie.
        if (options.depth != ColourDepth::Ascii) {
            con::enableAnsi();
            con::enableUtf8Output();
        }
        std::string frame;
        canvas.present(frame, options.depth, options.blocks, /*homeCursor=*/false);
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    // ── interactive ─────────────────────────────────────────────────────────
    con::InteractiveSession session;
    if (!session.ok()) {
        std::fprintf(stderr,
                     "omma-ascii could not take over the terminal.\n"
                     "  reason: %s\n"
                     "\n"
                     "Run it directly rather than through a pipe, or use\n"
                     "  omma-ascii --snapshot\n"
                     "for a single frame you can redirect.\n",
                     session.failureReason());
        return 1;
    }

    // NO_COLOR asks for no colour, not for no program. Drop to the monochrome
    // density-ramp presentation rather than refusing to render, unless the user
    // asked for a specific depth on the command line — an explicit flag is a
    // clearer statement of intent than an environment variable.
    if (con::colourDeclinedByEnvironment() && !options.depthForced) {
        options.depth = ColourDepth::Ascii;
    }

    // Half blocks are three UTF-8 bytes each. If the console cannot be put into
    // UTF-8, emitting them yields ten thousand copies of garbage per frame, so
    // fall back to one pixel per cell -- half the vertical resolution, and works
    // on any terminal ever made, because a space is a space everywhere.
    //
    // An explicit --blocks always wins: the user can see their own terminal and
    // we are only inferring.
    if (!options.blocksForced && !con::supportsUtf8()) {
        options.blocks = BlockStyle::FullCells;
    }

    World world{SolarSystem::standard(), std::chrono::seconds{1},
                Epoch::fromCivil(options.date)};

    // The cap is set every frame, not once. With no spacecraft it is effectively
    // infinite -- every body is analytic, so advancing the tick count is one
    // integer add whether it is by one or by a billion. The moment something is
    // being integrated it drops to what the RK4 kernel can finish inside a frame,
    // which is the promise the old comment here made.
    omma::StepPacer pacer{std::chrono::seconds{1}, 1'000'000'000'000LL};

    auto size = con::terminalSize();
    Canvas canvas{size.columns, size.rows};
    Camera camera = makePixelCamera(canvas, options);

    ViewState view{};
    view.focusIndex = focusIndexFor(world.system(), options.focus);
    for (int i = 0; i < options.preLaunch; ++i) {
        world.launch(makeLaunchRequest(BodyId::Earth, i + 1));
    }

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
            handleKey(key, view, camera, world);
        }
        if (camera_frame_hint) {
            camera.frame(kLowOrbitSpan);
            camera_frame_hint = false;
        }

        const auto frameStart = Clock::now();
        const double realDt = std::chrono::duration<double>(frameStart - lastFrame).count();
        lastFrame = frameStart;
        if (realDt > 0.0) {
            smoothedFps = 0.9 * smoothedFps + 0.1 / realDt;
        }

        pacer.setMaxStepsPerFrame(world.spacecraft().empty()
                                      ? 1'000'000'000'000LL
                                      : kIntegrationStepBudget);
        const double warp = view.paused ? 0.0 : kWarpLadder[view.warpIndex].factor;
        world.step(pacer.stepsForFrame(realDt, warp));
        clamped = pacer.lastFrameWasClamped();

        // Events are drained every frame whether or not anyone reads them, so the
        // queue cannot grow without bound in a long session.
        static_cast<void>(world.drainEvents());

        const auto current = con::terminalSize();
        if (current.columns != size.columns || current.rows != size.rows) {
            size = current;
            canvas.resize(size.columns, size.rows);
            camera.setViewport(canvas.pixelWidth(), canvas.pixelHeight());
            std::fputs(con::clearToEnd().data(), stdout);
        }

        const auto drawStart = Clock::now();
        render(canvas, camera, world, view, smoothedFps, clamped, timings);
        const auto presentStart = Clock::now();
        canvas.present(frameBuffer, options.depth, options.blocks, /*homeCursor=*/true);
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
