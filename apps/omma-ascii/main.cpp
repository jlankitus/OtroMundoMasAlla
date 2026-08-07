// ─────────────────────────────────────────────────────────────────────────────
// omma-ascii — the solar system, in your terminal.
//
// THE GAME LOOP, and where the determinism boundary sits inside it
//
//     while running:
//         realDt   = wall clock since last frame     <- nondeterministic
//         steps    = pacer.stepsForFrame(realDt, warp)
//         clock.step(steps)                          <- deterministic from here
//         handle input
//         render at clock.now()
//         sleep to cap the frame rate
//
// Everything above the clock.step() line depends on how fast this machine
// happens to be. Everything below it depends only on the tick count. Record
// the step counts and you can reproduce the run exactly.
//
// WHY STEPPING A MILLION TIMES IS FREE HERE
// At a million times warp the pacer asks for a million steps in one frame.
// SimClock::step(n) is a single integer addition, and every body in this scene
// is a Kepler ephemeris — a pure function of time. So no per-step work exists
// to be done; we advance the tick count and sample once at the new epoch.
//
// This is not a shortcut, it is the on-rails argument made concrete. The cost
// of warp is the cost of whatever is being *integrated*, and right now that is
// nothing. Add a spacecraft under thrust and the maximum useful warp drops to
// whatever the integrator can chew through — which is exactly why real space
// games put coasting craft on rails.
// ─────────────────────────────────────────────────────────────────────────────

#include "render/camera.hpp"
#include "render/canvas.hpp"

#include "core/console.hpp"
#include "core/epoch.hpp"
#include "core/sim_clock.hpp"
#include "core/units.hpp"
#include "core/version.hpp"
#include "physics/orbital_elements.hpp"
#include "physics/solar_system.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace con = omma::console;
using omma::render::Camera;
using omma::render::Canvas;
using omma::render::Ink;
using omma::BodyId;
using omma::Epoch;
using omma::SolarSystem;
using omma::constants::kAu;
using omma::constants::kSecondsPerDay;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Presentation data: how each body looks. Kept as a table rather than a switch
// so adding a body is a one-line data change, not a code change.
// ─────────────────────────────────────────────────────────────────────────────
struct BodyStyle {
    char glyph;
    Ink  ink;
    Ink  orbitInk;
};

constexpr std::array<BodyStyle, static_cast<std::size_t>(BodyId::Count)> kStyles{{
    {'@', Ink::BrightYellow,  Ink::BrightBlack},   // Sun
    {'m', Ink::White,         Ink::BrightBlack},   // Mercury
    {'V', Ink::BrightYellow,  Ink::BrightBlack},   // Venus
    {'E', Ink::BrightCyan,    Ink::Blue},          // Earth
    {'.', Ink::BrightWhite,   Ink::BrightBlack},   // Moon
    {'M', Ink::BrightRed,     Ink::Red},           // Mars
    {'J', Ink::BrightYellow,  Ink::Yellow},        // Jupiter
    {'S', Ink::Yellow,        Ink::BrightBlack},   // Saturn
    {'U', Ink::BrightCyan,    Ink::Cyan},          // Uranus
    {'N', Ink::BrightBlue,    Ink::Blue},          // Neptune
    {'p', Ink::BrightBlack,   Ink::BrightBlack},   // Pluto
}};

// ─────────────────────────────────────────────────────────────────────────────
// Warp and zoom ladders. Discrete presets rather than a continuous multiplier:
// you almost always want "one day per second", not "37194 times".
// ─────────────────────────────────────────────────────────────────────────────
struct WarpStep {
    double      factor;
    const char* label;
};

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

struct ZoomPreset {
    double      metresAcross;
    const char* label;
};

constexpr std::array<ZoomPreset, 5> kZoomPresets{{
    {1.2e9,          "Earth-Moon"},
    {3.6e11,         "inner system"},
    {1.7e12,         "to Jupiter"},
    {9.5e12,         "to Neptune"},
    {1.6e13,         "everything"},
}};

// ─────────────────────────────────────────────────────────────────────────────

struct ViewState {
    std::size_t warpIndex{5};        // 1 day per second
    std::size_t focusIndex{0};       // the Sun
    bool        paused{false};
    bool        showOrbits{true};
    bool        showLabels{true};
    bool        showHelp{false};
    bool        running{true};
    omma::Vec3  panOffset{};
};

/// Draw the closed ellipse a body will trace around its parent.
///
/// Sampled in ECCENTRIC anomaly rather than uniformly in time. Uniform time
/// sampling clusters points at apoapsis, where the body dawdles, and leaves
/// the periapsis arc — the fast, tightly curved, visually interesting part —
/// full of gaps. Uniform eccentric anomaly spreads points evenly around the
/// figure, which is what you want for drawing it.
void drawOrbit(Canvas& canvas, const Camera& camera,
               const omma::KeplerEphemeris& body, Epoch t, Ink ink) {
    const auto elements = body.elementsAt(t);
    const omma::Vec3 parentOrigin =
        body.parent() != nullptr ? body.parent()->sample(t).position : omma::Vec3::zero();

    constexpr int kSamples = 512;
    const double a = elements.semiMajorAxis;
    const double e = elements.eccentricity;
    if (!(a > 0.0) || e >= 1.0) {
        return;
    }

    // Rotation from the perifocal plane into the reference frame. Computed
    // once for the whole ellipse rather than per point.
    const double cosO = std::cos(elements.longitudeOfAscendingNode);
    const double sinO = std::sin(elements.longitudeOfAscendingNode);
    const double cosW = std::cos(elements.argumentOfPeriapsis);
    const double sinW = std::sin(elements.argumentOfPeriapsis);
    const double cosI = std::cos(elements.inclination);
    const double m11 = cosO * cosW - sinO * sinW * cosI;
    const double m12 = -cosO * sinW - sinO * cosW * cosI;
    const double m21 = sinO * cosW + cosO * sinW * cosI;
    const double m22 = -sinO * sinW + cosO * cosW * cosI;

    const double b = a * std::sqrt(1.0 - e * e);

    for (int i = 0; i < kSamples; ++i) {
        const double E = omma::constants::kTwoPi * static_cast<double>(i)
                       / static_cast<double>(kSamples);
        const double xp = a * (std::cos(E) - e);
        const double yp = b * std::sin(E);

        const omma::Vec3 world{parentOrigin.x + m11 * xp + m12 * yp,
                               parentOrigin.y + m21 * xp + m22 * yp,
                               0.0};

        const auto p = camera.project(world);
        // Never let a trail paint over a body. Bodies are drawn after orbits,
        // but a body's own trail passes through its current position, and one
        // frame of a planet flickering into a dot reads as a rendering bug.
        if (p.onScreen && canvas.glyphAt(p.x, p.y) == ' ') {
            canvas.put(p.x, p.y, '.', ink);
        }
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

void drawHud(Canvas& canvas, const SolarSystem& system, const omma::SimClock& clock,
             const Camera& camera, const ViewState& view, double framesPerSecond,
             bool clamped) {
    const int h = canvas.height();
    const Epoch t = clock.now();
    const auto& focus = system[static_cast<BodyId>(view.focusIndex)];

    // Clear the strips the HUD occupies before writing into them. Orbit dots
    // showing through between the letters turns a readout into noise.
    canvas.fill(0, 0, canvas.width(), 1);
    canvas.fill(0, h - 2, canvas.width(), 2);
    canvas.fill(0, 2, 26, 10);

    // ── top bar ──────────────────────────────────────────────────────────────
    char line[256];
    std::snprintf(line, sizeof(line), " %s   %s ",
                  omma::versionBanner().data(), t.toString().c_str());
    canvas.text(1, 0, line, Ink::BrightWhite);

    std::snprintf(line, sizeof(line), "JD %.4f  |  T+%.1f d  |  %.0f fps ",
                  t.julianDate(), omma::toSeconds(clock.elapsed()) / kSecondsPerDay,
                  framesPerSecond);
    canvas.text(std::max(1, canvas.width() - static_cast<int>(std::string(line).size()) - 1),
                0, line, Ink::BrightBlack);

    // ── bottom bar ───────────────────────────────────────────────────────────
    const auto& warp = kWarpLadder[view.warpIndex];
    std::snprintf(line, sizeof(line), " warp %s%-12s (%.0fx)   scale %s across   focus %s",
                  view.paused ? "PAUSED " : "", warp.label, warp.factor,
                  formatDistance(camera.viewWidthMetres()).c_str(),
                  focus.name().data());
    canvas.text(1, h - 2, line, view.paused ? Ink::BrightYellow : Ink::BrightGreen);

    if (clamped) {
        canvas.text(canvas.width() - 22, h - 2, "[falling behind]", Ink::BrightRed);
    }

    canvas.text(1, h - 1,
                " space pause   -/= warp   [/] zoom   1-5 presets   wasd pan"
                "   tab focus   c centre   o orbits   ? help   q quit",
                Ink::BrightBlack);

    // ── focus panel ──────────────────────────────────────────────────────────
    const auto* kepler = dynamic_cast<const omma::KeplerEphemeris*>(&focus);
    int row = 2;
    canvas.text(1, row++, focus.name(), Ink::BrightWhite);
    if (kepler != nullptr) {
        const auto elements = kepler->elementsAt(t);
        const auto state = kepler->sampleRelativeToParent(t);
        const double gm = kepler->propagationGm();

        std::snprintf(line, sizeof(line), "  r      %s", formatDistance(state.position.norm()).c_str());
        canvas.text(1, row++, line, Ink::White);
        std::snprintf(line, sizeof(line), "  v      %.3f km/s", state.velocity.norm() / 1000.0);
        canvas.text(1, row++, line, Ink::White);
        std::snprintf(line, sizeof(line), "  a      %s", formatDistance(elements.semiMajorAxis).c_str());
        canvas.text(1, row++, line, Ink::White);
        std::snprintf(line, sizeof(line), "  e      %.5f", elements.eccentricity);
        canvas.text(1, row++, line, Ink::White);
        std::snprintf(line, sizeof(line), "  i      %.3f deg", omma::toDegrees(elements.inclination));
        canvas.text(1, row++, line, Ink::White);
        std::snprintf(line, sizeof(line), "  peri   %s", formatDistance(periapsisRadius(elements)).c_str());
        canvas.text(1, row++, line, Ink::White);
        std::snprintf(line, sizeof(line), "  apo    %s", formatDistance(apoapsisRadius(elements)).c_str());
        canvas.text(1, row++, line, Ink::White);

        const double periodDays = orbitalPeriod(elements, gm) / kSecondsPerDay;
        if (periodDays < 900.0) {
            std::snprintf(line, sizeof(line), "  period %.2f d", periodDays);
        } else {
            std::snprintf(line, sizeof(line), "  period %.2f y", periodDays / 365.25);
        }
        canvas.text(1, row++, line, Ink::White);
    } else {
        canvas.text(1, row++, "  the frame origin", Ink::BrightBlack);
    }

    // ── help overlay ─────────────────────────────────────────────────────────
    if (view.showHelp) {
        static constexpr const char* kHelp[] = {
            "+---------------------------------------------+",
            "|  space   pause / resume                     |",
            "|  - =     time warp down / up                |",
            "|  [ ]     zoom out / in                      |",
            "|  1 - 5   zoom presets                       |",
            "|  w a s d pan the camera                     |",
            "|  tab / n next body      p  previous body    |",
            "|  c       re-centre on the focused body      |",
            "|  o       orbit trails on / off              |",
            "|  l       labels on / off                    |",
            "|  ?       this panel      q / esc  quit      |",
            "+---------------------------------------------+",
        };
        const int boxWidth = 47;
        const int x = std::max(0, (canvas.width() - boxWidth) / 2);
        const int y = std::max(0, (canvas.height() - static_cast<int>(std::size(kHelp))) / 2);
        for (std::size_t i = 0; i < std::size(kHelp); ++i) {
            canvas.text(x, y + static_cast<int>(i), kHelp[i], Ink::BrightWhite);
        }
    }
}

void render(Canvas& canvas, Camera& camera, const SolarSystem& system,
            const omma::SimClock& clock, const ViewState& view,
            double framesPerSecond, bool clamped) {
    const Epoch t = clock.now();
    canvas.clear();

    const auto& focusBody = system[static_cast<BodyId>(view.focusIndex)];
    camera.setCentre(focusBody.sample(t).position + view.panOffset);

    // Orbits first, so bodies always draw on top of their own trails.
    if (view.showOrbits) {
        for (std::size_t i = 0; i < system.size(); ++i) {
            const auto* kepler =
                dynamic_cast<const omma::KeplerEphemeris*>(system.bodies()[i].get());
            if (kepler != nullptr) {
                drawOrbit(canvas, camera, *kepler, t, kStyles[i].orbitInk);
            }
        }
    }

    // Bodies next. All of them, before any label — otherwise an early label
    // can sit where a later planet needs to draw, and the planet vanishes.
    struct Placed { std::size_t index; int x; int y; };
    std::vector<Placed> visible;
    visible.reserve(system.size());

    for (std::size_t i = 0; i < system.size(); ++i) {
        const auto p = camera.project(system.bodies()[i]->sample(t).position);
        if (!p.onScreen) {
            continue;
        }
        visible.push_back({i, p.x, p.y});
    }

    // Zoomed out far enough, several bodies land on the same cell and only one
    // glyph survives. Which one should it be? Not "whichever came last in the
    // array" -- that is how the Sun ends up rendered as Venus. Sort by GM and
    // draw ascending, so the most massive body in a cell wins. The rule is
    // derived from the data rather than from a hand-written priority list, so
    // it stays correct when bodies are added.
    std::sort(visible.begin(), visible.end(),
              [&](const Placed& a, const Placed& b) {
                  return system.bodies()[a.index]->gravitationalParameter()
                       < system.bodies()[b.index]->gravitationalParameter();
              });

    for (const Placed& body : visible) {
        canvas.put(body.x, body.y, kStyles[body.index].glyph, kStyles[body.index].ink);
    }

    // Labels last, and only where they fit.
    //
    // Zoomed out to Neptune, the inner planets land within a couple of cells
    // of each other and naive labelling produces "V.VMoonhy" -- four names
    // overwriting one another into nonsense. Two rules fix it: never draw over
    // an occupied cell, and keep labels apart from each other. The focused
    // body is placed first so it always wins the space.
    if (view.showLabels) {
        std::vector<Placed> labelled;
        auto tryLabel = [&](const Placed& body) {
            const std::string_view name = system.bodies()[body.index]->name();
            const bool isFocus = body.index == view.focusIndex;

            for (const Placed& other : labelled) {
                if (other.y == body.y && std::abs(other.x - body.x) < 12) {
                    return;   // another name already owns this neighbourhood
                }
            }
            const int length = static_cast<int>(name.size());
            if (!canvas.spanIsClear(body.x + 2, body.y, length)) {
                return;
            }
            canvas.text(body.x + 2, body.y, name,
                        isFocus ? Ink::BrightWhite : Ink::BrightBlack);
            labelled.push_back(body);
        };

        for (const Placed& body : visible) {
            if (body.index == view.focusIndex) {
                tryLabel(body);
            }
        }
        for (const Placed& body : visible) {
            if (body.index != view.focusIndex) {
                tryLabel(body);
            }
        }
    }

    drawHud(canvas, system, clock, camera, view, framesPerSecond, clamped);
}

void handleKey(int key, ViewState& view, Camera& camera, const SolarSystem& system) {
    const double panStep = camera.metresPerRow() * 6.0;

    switch (key) {
        case 'q': case 'Q': case 0x1b: case 0x03:   // q, Esc, Ctrl-C
            view.running = false;
            break;
        case ' ':
            view.paused = !view.paused;
            break;

        case '=': case '+':
            if (view.warpIndex + 1 < kWarpLadder.size()) ++view.warpIndex;
            break;
        case '-': case '_':
            if (view.warpIndex > 0) --view.warpIndex;
            break;

        case ']':
            camera.zoomBy(1.0 / 1.35);
            break;
        case '[':
            camera.zoomBy(1.35);
            break;

        case '1': case '2': case '3': case '4': case '5': {
            const auto index = static_cast<std::size_t>(key - '1');
            camera.frame(kZoomPresets[index].metresAcross);
            break;
        }

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
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Command line.
//
// --snapshot exists so the renderer can be verified without a human watching
// it. A visual system that can only be checked by looking at it is a visual
// system nobody checks, and it silently rots. One frame, plain text, on
// stdout: diffable, greppable, and something CI can assert against.
// ─────────────────────────────────────────────────────────────────────────────
struct Options {
    bool        snapshot{false};
    bool        colour{true};
    int         columns{0};        // 0 means "ask the terminal"
    int         rows{0};
    std::size_t zoomPreset{4};     // 1-5, stored 0-based
    std::string focus{"Sun"};
    omma::CivilTime date{2026, 8, 6, 12, 0, 0.0};
    bool        showHelpAndExit{false};
};

void printUsage() {
    std::puts(
        "omma-ascii - the solar system, in your terminal\n"
        "\n"
        "  (no arguments)      run interactively\n"
        "  --snapshot          render one frame to stdout and exit\n"
        "  --no-colour         plain text, no ANSI escapes\n"
        "  --size WxH          override the terminal size\n"
        "  --zoom N            zoom preset 1-5 (Earth-Moon .. everything)\n"
        "  --focus NAME        centre on a body, e.g. Earth\n"
        "  --date YYYY-MM-DD   start epoch\n"
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
        } else if (arg == "--no-colour" || arg == "--no-color") {
            out.colour = false;
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

    // ── snapshot: one frame, no terminal takeover ────────────────────────────
    if (options.snapshot) {
        const int columns = options.columns > 0 ? options.columns : 110;
        const int rows = options.rows > 0 ? options.rows : 34;

        Canvas canvas{columns, rows};
        Camera camera{columns, rows};
        camera.frame(kZoomPresets[options.zoomPreset].metresAcross);

        omma::SimClock snapshotClock{std::chrono::seconds{1}, Epoch::fromCivil(options.date)};

        ViewState view{};
        view.focusIndex = focusIndexFor(system, options.focus);

        render(canvas, camera, system, snapshotClock, view, 30.0, false);

        std::string frame;
        canvas.present(frame, options.colour && con::enableAnsi(), /*homeCursor=*/false);
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    // ── interactive ──────────────────────────────────────────────────────────
    con::InteractiveSession session;
    if (!session.ok()) {
        std::fprintf(stderr,
                     "omma-ascii needs an interactive terminal.\n"
                     "Run it directly rather than through a pipe, or use\n"
                     "  omma-ascii --snapshot\n"
                     "for a single frame you can redirect.\n");
        return 1;
    }

    // One simulated second per tick. Note this is completely independent of
    // the frame rate; see the header comment.
    omma::SimClock clock{std::chrono::seconds{1}, Epoch::fromCivil(options.date)};

    // maxStepsPerFrame is deliberately enormous. That clamp exists to bound
    // INTEGRATION work per frame, and nothing here is being integrated -- every
    // body is analytic, so advancing the tick count is one integer add whether
    // it is by one or by a billion. When spacecraft arrive this comes back down
    // to something the RK4 kernel can actually finish inside a frame.
    omma::StepPacer pacer{std::chrono::seconds{1}, 1'000'000'000'000LL};

    auto size = con::terminalSize();
    Canvas canvas{size.columns, size.rows};
    Camera camera{size.columns, size.rows};
    camera.frame(kZoomPresets[options.zoomPreset].metresAcross);

    ViewState view{};
    view.focusIndex = focusIndexFor(system, options.focus);
    std::string frameBuffer;
    frameBuffer.reserve(static_cast<std::size_t>(size.columns * size.rows) * 4);

    using Clock = std::chrono::steady_clock;
    auto lastFrame = Clock::now();
    double smoothedFps = 60.0;
    bool clamped = false;

    while (view.running) {
        // ── input ────────────────────────────────────────────────────────────
        // Drain the queue rather than reading one key per frame; a held key
        // otherwise builds a backlog that keeps acting long after release.
        while (const int key = con::pollKey()) {
            handleKey(key, view, camera, system);
        }

        // ── wall clock -> simulated steps ────────────────────────────────────
        const auto frameStart = Clock::now();
        const double realDt =
            std::chrono::duration<double>(frameStart - lastFrame).count();
        lastFrame = frameStart;

        if (realDt > 0.0) {
            smoothedFps = 0.9 * smoothedFps + 0.1 / realDt;
        }

        const double warp = view.paused ? 0.0 : kWarpLadder[view.warpIndex].factor;
        clock.step(pacer.stepsForFrame(realDt, warp));
        clamped = pacer.lastFrameWasClamped();

        // ── resize ───────────────────────────────────────────────────────────
        const auto current = con::terminalSize();
        if (current.columns != size.columns || current.rows != size.rows) {
            size = current;
            canvas.resize(size.columns, size.rows);
            camera.setViewport(size.columns, size.rows);
            std::fputs(con::clearToEnd().data(), stdout);
        }

        // ── draw ─────────────────────────────────────────────────────────────
        render(canvas, camera, system, clock, view, smoothedFps, clamped);
        canvas.present(frameBuffer, options.colour, /*homeCursor=*/true);
        std::fwrite(frameBuffer.data(), 1, frameBuffer.size(), stdout);
        std::fflush(stdout);

        // ── pace the frame ───────────────────────────────────────────────────
        // Sleeping the remainder of a 30 Hz budget rather than spinning. There
        // is no reason to burn a core redrawing a terminal faster than anyone
        // can read it, and the simulation rate does not depend on this number.
        constexpr auto kFrameBudget = std::chrono::duration<double>{1.0 / 30.0};
        const auto spent = std::chrono::duration<double>{Clock::now() - frameStart};
        if (spent < kFrameBudget) {
            std::this_thread::sleep_for(kFrameBudget - spent);
        }
    }

    return 0;
}
