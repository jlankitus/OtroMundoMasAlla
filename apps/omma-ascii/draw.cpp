#include "draw.hpp"

#include "bodies.hpp"

#include "core/units.hpp"
#include "core/version.hpp"
#include "physics/kepler_ephemeris.hpp"
#include "physics/orbital_elements.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace omma::app {
namespace {

using constants::kAu;
using constants::kSecondsPerDay;
using render::Camera;
using render::Canvas;
using render::Ink;
using render::Rgb;
using render::ScreenPoint;

// The spacecraft is deliberately the brightest thing on screen after the Sun.
// The planets are scenery; the craft is the subject, and at one or two pixels
// across it needs every bit of contrast it can get.
constexpr Rgb kCraftColour{120, 255, 170};      // coasting
constexpr Rgb kCraftBurning{255, 190, 90};      // engine lit
constexpr Rgb kCraftDead{110, 90, 90};          // wreckage
constexpr Rgb kCraftOrbit{40, 150, 110};        // predicted path
constexpr Rgb kCraftOrbitFocused{90, 240, 180};

/// Draw a closed orbit ellipse as a polyline.
///
/// Sampled in ECCENTRIC anomaly, not time: uniform time clusters samples at
/// apoapsis and leaves the fast periapsis arc full of gaps. Drawn as segments,
/// not points, so 256 samples read as a curve at any zoom.
void drawEllipse(Canvas& canvas, const Camera& camera,
                 const OrbitalElements& elements,
                 const Vec3& parentOrigin, Rgb colour) {
    const double a = elements.semiMajorAxis;
    const double e = elements.eccentricity;
    if (!(a > 0.0) || e >= 1.0) {
        return;
    }

    // Cull orbits that carry no information at this zoom: much wider than the
    // view they render as a straight line across it, much smaller than a pixel
    // they render as noise. The upper bound is generous because an orbit a few
    // times wider than the view still reads as a useful arc.
    const double viewSpan = camera.viewWidthMetres();
    if (a > viewSpan * 25.0 || a < camera.metresPerRow() * 1.5) {
        return;
    }

    // Perifocal-to-reference rotation, computed once for the whole ellipse.
    // Full 3x3: the z component is the entire reason inclination is visible.
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
    ScreenPoint previous{};
    bool havePrevious = false;

    for (int i = 0; i <= kSamples; ++i) {
        const double E = constants::kTwoPi * static_cast<double>(i)
                       / static_cast<double>(kSamples);
        const double xp = a * (std::cos(E) - e);
        const double yp = b * std::sin(E);

        const Vec3 world{parentOrigin.x + m11 * xp + m12 * yp,
                         parentOrigin.y + m21 * xp + m22 * yp,
                         parentOrigin.z + m31 * xp + m32 * yp};

        // Draw whenever both endpoints are representable and let Canvas::line
        // clip. Testing "is either endpoint on screen" here silently drops
        // every chord that straddles the viewport — clipping is the renderer's
        // job, not the caller's.
        const auto point = camera.project(world);
        if (havePrevious && point.valid && previous.valid) {
            canvas.line(previous.x, previous.y, point.x, point.y, colour);
        }
        previous = point;
        havePrevious = true;
    }
}

void drawOrbit(Canvas& canvas, const Camera& camera,
               const KeplerEphemeris& body, Epoch t, Rgb colour) {
    drawEllipse(canvas, camera, body.elementsAt(t),
                body.parent() != nullptr ? body.parent()->sample(t).position
                                         : Vec3::zero(),
                colour);
}

/// A spacecraft's predicted path: the osculating ("kissing") ellipse matching
/// its current state — where it would go if nothing else ever touched it. It
/// jumps the instant the engine lights, and watching it deform is the feedback
/// that makes flying by hand possible.
void drawPredictedPath(Canvas& canvas, const Camera& camera, const World& world,
                       const Spacecraft& craft, bool focused) {
    const auto& central = *world.system().bodies()[craft.centralBodyIndex];
    drawEllipse(canvas, camera, world.elementsOf(craft),
                central.sample(world.now()).position,
                focused ? kCraftOrbitFocused : kCraftOrbit);
}

/// Apparent radius in pixels: true to scale, floored at one pixel so a planet
/// never vanishes just because it is small on screen.
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
                    dynamic_cast<const KeplerEphemeris*>(system.bodies()[i].get());
                if (kepler == nullptr) {
                    continue;
                }
                drawOrbit(canvas, camera, *kepler, t, orbitColourFor(i, focused));
            }
        }
    }

    // Painter's algorithm: collect, sort by camera depth, draw nearest last.
    // Sorting by GM would put the Sun on top even when Mercury passes in
    // front of it.
    struct Visible {
        std::size_t index;
        ScreenPoint point;
        double depth;
        int radius;
    };
    std::vector<Visible> visible;
    visible.reserve(system.size());

    for (std::size_t i = 0; i < system.size(); ++i) {
        const auto& body = *system.bodies()[i];
        const Vec3 position = body.sample(t).position;
        const auto point = camera.project(position);
        if (!point.onScreen) {
            continue;
        }
        visible.push_back({i, point, camera.depthOf(position),
                           apparentRadiusPixels(body.meanRadius(), metresPerPixel)});
    }

    std::sort(visible.begin(), visible.end(),
              [](const Visible& a, const Visible& b) { return a.depth < b.depth; });

    // All glow first, so one body's corona cannot wash out another's disc.
    for (const Visible& v : visible) {
        const BodyStyle& style = styleFor(v.index);
        if (style.glowScale > 0.0) {
            const int radius = std::max(3, static_cast<int>(
                static_cast<double>(v.radius) * style.glowScale));
            canvas.glow(v.point.x, v.point.y, radius, style.colour.scaled(0.55));
        }
    }

    // Bodies, shaded as lit spheres. The light direction is taken in SCREEN
    // space (project the Sun, project the body, subtract) with a constant lean
    // toward the viewer for the z component — indistinguishable from the exact
    // rotate-into-camera-basis version at these sizes, for two projections
    // instead of a matrix.
    const auto sunScreen = camera.project(system[BodyId::Sun].sample(t).position);

    for (const Visible& v : visible) {
        const Rgb colour = styleFor(v.index).colour;

        if (v.index == static_cast<std::size_t>(BodyId::Sun) || v.radius <= 1) {
            // The Sun is the light source; it has no dark side to show.
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

    // Spacecraft after the planets, so a satellite is never hidden behind the
    // body it orbits, with a glow while burning so a lit engine reads even
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

    // Body labels last, and only where they fit. Zoomed out, the inner planets
    // land within pixels of each other and naive labelling overwrites names
    // into nonsense — so never draw over an occupied cell, keep labels apart,
    // and place the focused body first so it always wins the space.
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

/// One label/value row of the focus panel: label dim, value bright — the
/// numbers are what you are reading, and a uniform grey makes you hunt.
void panelRow(Canvas& canvas, int row, const char* label, const std::string& value) {
    char text[32];
    std::snprintf(text, sizeof(text), "  %-8s", label);
    canvas.text(1, row, text, Ink::BrightBlack);
    canvas.text(11, row, value, Ink::BrightCyan);
}

void drawHud(Canvas& canvas, const World& world, const Camera& camera,
             const ViewState& view, double framesPerSecond, bool clamped,
             const FrameTimings& timings) {
    const SolarSystem& system = world.system();
    const SimClock& clock = world.clock();
    const int h = canvas.height();
    const Epoch t = clock.now();

    const Spacecraft* craft = focusedCraft(world, view);
    const std::size_t bodyIndex = std::min(view.focusIndex, system.size() - 1);
    const auto& focus = *system.bodies()[bodyIndex];
    const auto* kepler = craft != nullptr
                             ? nullptr
                             : dynamic_cast<const KeplerEphemeris*>(&focus);

    // Clear the strips the HUD occupies — pixels showing through between the
    // letters turn a readout into noise — but only the rows the panel writes.
    canvas.fill(0, 0, canvas.width(), 1);
    canvas.fill(0, h - 2, canvas.width(), 2);
    canvas.fill(0, 2, 30, (kepler != nullptr || craft != nullptr) ? 12 : 2);

    char line[256];
    std::snprintf(line, sizeof(line), " %s   %s ",
                  versionBanner().data(), t.toString().c_str());
    canvas.text(1, 0, line, Ink::BrightWhite);

    std::snprintf(line, sizeof(line), "T+%.1f d  |  %.0f fps ",
                  toSeconds(clock.elapsed()) / kSecondsPerDay, framesPerSecond);
    canvas.text(std::max(1, canvas.width() - static_cast<int>(std::string(line).size()) - 1),
                0, line, Ink::BrightBlack);

    const auto& warp = kWarpLadder[view.warpIndex];
    std::snprintf(line, sizeof(line),
                  " warp %s%-11s   scale %s   tilt %.0f deg   spin %.0f deg   focus %s",
                  view.paused ? "PAUSED " : "", warp.label,
                  formatDistance(camera.viewWidthMetres()).c_str(),
                  toDegrees(camera.elevation()),
                  toDegrees(camera.azimuth()),
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

    auto formatted = [](const char* format, double value) {
        char b[24];
        std::snprintf(b, sizeof(b), format, value);
        return std::string{b};
    };

    if (craft != nullptr) {
        canvas.text(1, row++, craft->name, Ink::BrightWhite);
        const auto elements = world.elementsOf(*craft);
        const auto& central = *system.bodies()[craft->centralBodyIndex];
        const double gm = central.gravitationalParameter();

        panelRow(canvas, row++, "orbits", std::string{central.name()});
        panelRow(canvas, row++, "alt",    formatDistance(world.altitudeOf(*craft)));
        panelRow(canvas, row++, "peri",
                 formatDistance(periapsisRadius(elements) - central.meanRadius()));
        panelRow(canvas, row++, "apo",
                 formatDistance(apoapsisRadius(elements) - central.meanRadius()));
        panelRow(canvas, row++, "e",      formatted("%.5f", elements.eccentricity));
        panelRow(canvas, row++, "i",      formatted("%.2f deg",
                                                    toDegrees(elements.inclination)));
        panelRow(canvas, row++, "period", formatted("%.1f min",
                                                    orbitalPeriod(elements, gm) / 60.0));
        panelRow(canvas, row++, "prop",   formatted("%.1f kg", craft->propellantKg));
        panelRow(canvas, row++, "dv left", formatted("%.0f m/s",
                                                     craft->remainingDeltaVMps()));
        panelRow(canvas, row++, "dv used", formatted("%.1f m/s", craft->deltaVSpentMps));

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

        panelRow(canvas, row++, "r",    formatDistance(state.position.norm()));
        panelRow(canvas, row++, "v",    formatted("%.3f km/s",
                                                  state.velocity.norm() / 1000.0));
        panelRow(canvas, row++, "a",    formatDistance(elements.semiMajorAxis));
        panelRow(canvas, row++, "e",    formatted("%.5f", elements.eccentricity));
        panelRow(canvas, row++, "i",    formatted("%.3f deg",
                                                  toDegrees(elements.inclination)));
        panelRow(canvas, row++, "peri", formatDistance(periapsisRadius(elements)));
        panelRow(canvas, row++, "apo",  formatDistance(apoapsisRadius(elements)));
        const double days =
            orbitalPeriod(elements, kepler->propagationGm()) / kSecondsPerDay;
        panelRow(canvas, row++, "period",
                 days < 900.0 ? formatted("%.2f d", days)
                              : formatted("%.2f y", days / 365.25));
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

}  // namespace

void render(Canvas& canvas, Camera& camera, const World& world,
            const ViewState& view, double framesPerSecond, bool clamped,
            const FrameTimings& timings) {
    canvas.clear();
    // Deep space rather than pure black: a hint of blue reads as sky instead
    // of as a hole, and gives the dimmest orbit line something to sit against.
    for (int py = 0; py < canvas.pixelHeight(); ++py) {
        for (int px = 0; px < canvas.pixelWidth(); ++px) {
            canvas.setPixel(px, py, kSpace);
        }
    }
    camera.setCentre(focusPosition(world, view) + view.panOffset);
    drawScene(canvas, camera, world, view);
    drawHud(canvas, world, camera, view, framesPerSecond, clamped, timings);
}

}  // namespace omma::app
