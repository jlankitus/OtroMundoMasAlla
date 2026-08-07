// ─────────────────────────────────────────────────────────────────────────────
// console — the one file in the project allowed to contain #ifdef _WIN32.
//
// Windows consoles do not interpret ANSI escape sequences unless the process
// asks for it, and even then only on Windows 10 1511 and later. Rather than
// sprinkle platform checks through the renderer, we ask once at startup and
// expose a single bool. Everything downstream just reads `Console::supportsAnsi()`.
//
// This is the Facade pattern doing its actual job: not hiding complexity for
// aesthetics, but confining a platform difference to a place where it can be
// tested and reasoned about.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string_view>

namespace omma::console {

/// Enable ANSI/VT escape sequence processing on this terminal if possible, and
/// report whether colour output will actually work. Safe to call more than
/// once; only the first call does anything.
///
/// Returns false when output is redirected to a file or pipe, so that piping
/// the headless app into a log does not fill it with escape codes.
bool enableAnsi() noexcept;

/// Result of the most recent enableAnsi(). False until it has been called.
[[nodiscard]] bool supportsAnsi() noexcept;

/// Escape sequences that evaluate to "" when the terminal cannot render them,
/// so call sites need no conditionals.
[[nodiscard]] std::string_view reset() noexcept;
[[nodiscard]] std::string_view bold() noexcept;
[[nodiscard]] std::string_view dim() noexcept;
[[nodiscard]] std::string_view red() noexcept;
[[nodiscard]] std::string_view green() noexcept;
[[nodiscard]] std::string_view yellow() noexcept;
[[nodiscard]] std::string_view blue() noexcept;
[[nodiscard]] std::string_view magenta() noexcept;
[[nodiscard]] std::string_view cyan() noexcept;
[[nodiscard]] std::string_view white() noexcept;

}  // namespace omma::console
