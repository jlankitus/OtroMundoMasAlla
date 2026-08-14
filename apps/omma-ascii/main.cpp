// ─────────────────────────────────────────────────────────────────────────────
// omma-ascii — the solar system, in your terminal, on a tilted board.
//
// The game loop, and where the determinism boundary sits inside it:
//
//     while running:
//         realDt = wall clock since last frame        <- nondeterministic
//         steps  = pacer.stepsForFrame(realDt, warp)
//         world.step(steps)                           <- deterministic below here
//         handle input
//         render at world.now()
//         sleep out the rest of the budget
//
// Everything above world.step() depends on how fast this machine happens to
// be; everything below depends only on the tick count, so a recorded sequence
// of step counts replays identically anywhere. --record swaps the two
// nondeterministic inputs (wall clock, keyboard) for scripted ones and runs
// this same loop headlessly. See docs/DESIGN.md §3 and docs/QA.md.
// ─────────────────────────────────────────────────────────────────────────────

#include "draw.hpp"
#include "options.hpp"
#include "scenarios.hpp"
#include "view.hpp"

#include "core/console.hpp"
#include "core/sim_clock.hpp"
#include "core/units.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace con = omma::console;
using namespace omma::app;
using omma::BodyId;
using omma::Epoch;
using omma::SolarSystem;
using omma::Spacecraft;
using omma::ThrustCommand;
using omma::World;
using omma::render::BlockStyle;
using omma::render::Camera;
using omma::render::Canvas;
using omma::render::ColourDepth;

namespace {

/// Put a satellite in low orbit around whichever body is in view, then follow
/// and frame it — launching something and having to hunt for it is a bad
/// first five seconds. Focus on a craft or the Sun falls back to Earth,
/// because refusing to launch would be technically defensible and annoying.
void launchFromFocus(World& world, ViewState& view, int index) {
    const std::size_t bodies = world.system().size();
    std::size_t around = view.focusIndex < bodies ? view.focusIndex
                                                  : static_cast<std::size_t>(BodyId::Earth);
    if (around == static_cast<std::size_t>(BodyId::Sun)) {
        around = static_cast<std::size_t>(BodyId::Earth);
    }

    const auto id = world.launch(makeLaunchRequest(static_cast<BodyId>(around), index));
    if (id.isValid()) {
        view.focusIndex = bodies + world.spacecraft().size() - 1;
        view.panOffset = omma::Vec3::zero();
        view.frameRequested = true;
    }
}

/// Fire the focused craft's engine for 10 m/s: small enough to steer with,
/// big enough to see the predicted path move. Ignored when the focus is not
/// a living spacecraft, rather than beeping about it.
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

        // Uppercase L launches; lowercase l toggles labels. "Launch" deserves
        // to be the shifted one: it is the only key here that permanently
        // changes the world, and a stray keypress should not put a satellite
        // in orbit.
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

std::size_t focusIndexFor(const SolarSystem& system, std::string_view name) {
    for (std::size_t i = 0; i < system.size(); ++i) {
        if (system.bodies()[i]->name() == name) {
            return i;
        }
    }
    return 0;
}

/// Build the world, apply the scenario, and point the camera at what it
/// wants. Shared by all three entry points, so the QA harness renders the
/// same starting state the interactive binary shows — the only way its
/// results mean anything.
void setUpWorld(World& world, Camera& camera, ViewState& view, const Options& options) {
    const ScenarioView wanted = applyScenario(world, options.scenario);

    for (int i = 0; i < options.preLaunch; ++i) {
        world.launch(makeLaunchRequest(BodyId::Earth, i + 1));
    }

    // An explicit --focus or --zoom always wins. The scenario is a default,
    // and a default that overrides what the user typed is a bug.
    view.focusIndex = options.focusForced
                          ? focusIndexFor(world.system(), options.focus)
                          : std::min(wanted.focusIndex, focusTargetCount(world) - 1);

    if (!options.zoomForced && wanted.frameSpan > 0.0) {
        camera.frame(wanted.frameSpan);
    }
    view.warpIndex = std::min(wanted.warpIndex, kWarpLadder.size() - 1);

    // Settle last, so the scenario's craft exist to be advanced.
    if (options.settleSteps > 0) {
        world.step(options.settleSteps);
        static_cast<void>(world.drainEvents());
    }
}

/// A Camera measured in half-block PIXELS, not character cells: two pixel
/// rows per cell and each pixel square, so the unit aspect is 1.0, not the
/// 2.0 a character grid needs. Pass 2.0 here and every orbit is an egg.
Camera makePixelCamera(const Canvas& canvas, const Options& options) {
    Camera camera{canvas.pixelWidth(), canvas.pixelHeight(), /*unitAspect=*/1.0};
    camera.setElevation(omma::toRadians(options.elevationDeg));
    camera.setAzimuth(omma::toRadians(options.azimuthDeg));
    camera.frame(kZoomPresets[options.zoomPreset].metresAcross);
    return camera;
}

World makeWorld(const Options& options) {
    return World{SolarSystem::standard(), std::chrono::seconds{1},
                 Epoch::fromCivil(options.date)};
}

/// Run the live loop headlessly and write every frame's bytes to disk.
/// Timings are zeroed rather than measured and the reported fps is pinned to
/// the synthetic rate: a recording must not depend on the machine writing it,
/// and a paused recording being byte-identical frame to frame is what turns
/// "does it flicker" into a one-line check.
int runRecording(const Options& options) {
    const int columns = options.columns > 0 ? options.columns : 120;
    const int rows = options.rows > 0 ? options.rows : 36;

    Canvas canvas{columns, rows};
    Camera camera = makePixelCamera(canvas, options);
    World world = makeWorld(options);
    omma::StepPacer pacer{std::chrono::seconds{1}, kUnboundedStepBudget};

    ViewState view{};
    view.paused = options.startPaused;
    setUpWorld(world, camera, view, options);

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
        if (static_cast<std::size_t>(i) < options.keys.size()) {
            const char scripted = options.keys[static_cast<std::size_t>(i)];
            if (scripted != '.') {
                // '@' is the script's spelling of the prograde-burn key,
                // because '.' means "no key this frame". Translate AFTER the
                // no-key test — the other order swallows every scripted burn.
                const int key = (scripted == '@') ? '.' : scripted;
                handleKey(key, view, camera, world);
                if (!view.running) {
                    break;
                }
            }
        }
        if (view.frameRequested) {
            camera.frame(kLowOrbitSpan);
            view.frameRequested = false;
        }

        pacer.setMaxStepsPerFrame(stepBudgetFor(world));
        const double warp = view.paused ? 0.0 : kWarpLadder[view.warpIndex].factor;
        world.step(pacer.stepsForFrame(options.frameDeltaSeconds, warp));

        render(canvas, camera, world, view, reportedFps,
               pacer.lastFrameWasClamped(), FrameTimings{});
        canvas.present(frame, options.depth, options.blocks, /*homeCursor=*/true);

        char name[64];
        std::snprintf(name, sizeof(name), "frame_%05d.bin", i);
        const std::filesystem::path path =
            std::filesystem::path{options.recordDir} / name;

        // Binary, so nothing translates a newline on the way to disk. A
        // recording that differs from what the terminal would have received
        // is not a recording of anything.
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

/// Render one frame to stdout and exit: plain text, diffable, assertable.
int runSnapshot(const Options& options) {
    Canvas canvas{options.columns > 0 ? options.columns : 110,
                  options.rows > 0 ? options.rows : 34};
    Camera camera = makePixelCamera(canvas, options);
    World world = makeWorld(options);

    ViewState view{};
    setUpWorld(world, camera, view, options);

    render(canvas, camera, world, view, 30.0, false, FrameTimings{});

    // Colour is honoured as asked even when piped: a snapshot is often
    // captured deliberately, and forcing plain text would make --colour a lie.
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

int runInteractive(Options& options) {
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

    // NO_COLOR asks for no colour, not for no program: drop to the monochrome
    // density ramp unless an explicit --colour said otherwise.
    if (con::colourDeclinedByEnvironment() && !options.depthForced) {
        options.depth = ColourDepth::Ascii;
    }

    // Half blocks are three UTF-8 bytes each; on a console that cannot be put
    // into UTF-8 they render as garbage, so fall back to one pixel per cell.
    // An explicit --blocks always wins — the user can see their own terminal.
    if (!options.blocksForced && !con::supportsUtf8()) {
        options.blocks = BlockStyle::FullCells;
    }

    World world = makeWorld(options);
    omma::StepPacer pacer{std::chrono::seconds{1}, kUnboundedStepBudget};

    auto size = con::terminalSize();
    Canvas canvas{size.columns, size.rows};
    Camera camera = makePixelCamera(canvas, options);

    ViewState view{};
    setUpWorld(world, camera, view, options);

    std::string frameBuffer;
    frameBuffer.reserve(static_cast<std::size_t>(size.columns * size.rows) * 24);

    using Clock = std::chrono::steady_clock;
    auto lastFrame = Clock::now();
    double smoothedFps = 30.0;
    FrameTimings timings{};

    while (view.running) {
        // Drain the key queue rather than reading one per frame; a held key
        // otherwise builds a backlog that keeps acting long after release.
        while (const int key = con::pollKey()) {
            handleKey(key, view, camera, world);
        }
        if (view.frameRequested) {
            camera.frame(kLowOrbitSpan);
            view.frameRequested = false;
        }

        const auto frameStart = Clock::now();
        const double realDt = std::chrono::duration<double>(frameStart - lastFrame).count();
        lastFrame = frameStart;
        if (realDt > 0.0) {
            smoothedFps = 0.9 * smoothedFps + 0.1 / realDt;
        }

        // The step cap is set every frame: effectively infinite while every
        // body is analytic, dropping to what the RK4 kernel can finish inside
        // a frame the moment something is being integrated.
        pacer.setMaxStepsPerFrame(stepBudgetFor(world));
        const double warp = view.paused ? 0.0 : kWarpLadder[view.warpIndex].factor;
        world.step(pacer.stepsForFrame(realDt, warp));
        const bool clamped = pacer.lastFrameWasClamped();

        // Drained every frame whether or not anyone reads them, so the queue
        // cannot grow without bound in a long session.
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

        // con::sleepFor, not std::this_thread::sleep_for: Windows' 15.625 ms
        // timer tick rounds a portable sleep up hard enough to turn a 30 fps
        // target into 21, and the renderer gets blamed for it.
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

}  // namespace

int main(int argc, char** argv) {
    // Before anything is written: a renderer that controls the cursor has to
    // control the bytes, and Windows text mode rewrites \n to \r\n.
    con::useBinaryStdout();

    Options options{};
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }
    if (options.showHelpAndExit) {
        printUsage();
        return 0;
    }
    if (options.recordFrames > 0) {
        return runRecording(options);
    }
    if (options.snapshot) {
        return runSnapshot(options);
    }
    return runInteractive(options);
}
