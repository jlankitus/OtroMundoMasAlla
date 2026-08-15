#include "options.hpp"

#include "view.hpp"

#include <cstdio>
#include <cstdlib>

namespace omma::app {

using render::BlockStyle;
using render::ColourDepth;

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
        "  --launch N          put N extra satellites in low Earth orbit\n"
        "  --scenario NAME     empty, leo (default), constellation, or transfer\n"
        "  --settle N          run N simulated steps before the first frame\n"
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
            out.zoomForced = true;
        } else if (arg == "--focus") {
            out.focus = next();
            out.focusForced = true;
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
        } else if (arg == "--scenario") {
            const std::string name = next();
            if (const auto scenario = omma::scenarioFromName(name)) {
                out.scenario = *scenario;
            } else {
                std::fprintf(stderr,
                             "--scenario wants empty, leo, constellation or transfer\n");
                return false;
            }
        } else if (arg == "--settle") {
            out.settleSteps = std::atoi(next().c_str());
            if (out.settleSteps < 0) {
                std::fprintf(stderr, "--settle wants a step count >= 0\n");
                return false;
            }
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

}  // namespace omma::app
