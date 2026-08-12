// ─────────────────────────────────────────────────────────────────────────────
// console — the one file in the project allowed to contain #ifdef _WIN32.
// Windows consoles interpret ANSI escapes only when asked (Windows 10 1511+),
// so we ask once at startup; everything downstream reads supportsAnsi().
// Facade pattern confining the platform difference. See docs/DESIGN.md §9.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <chrono>
#include <string_view>

namespace omma::console {

/// Enable ANSI/VT escape processing and report whether colour output will
/// work. Only the first call does anything. Returns false when output is
/// redirected, so piping into a log does not fill it with escape codes.
bool enableAnsi() noexcept;

/// Result of the most recent enableAnsi(). False until it has been called.
[[nodiscard]] bool supportsAnsi() noexcept;

/// Enable escape-sequence processing, and report whether the console will honour
/// it. Separate from enableAnsi() on purpose: escapes are not colour. NO_COLOR
/// (https://no-color.org) asks for no colour, not for no cursor control —
/// conflating the two once made the renderer refuse to run under NO_COLOR=1.
bool enableVirtualTerminal() noexcept;

/// Result of the most recent enableVirtualTerminal().
[[nodiscard]] bool supportsVirtualTerminal() noexcept;

/// True when NO_COLOR is set. Callers should drop to monochrome output, not
/// refuse to run.
[[nodiscard]] bool colourDeclinedByEnvironment() noexcept;

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

// ─────────────────────────────────────────────────────────────────────────────
// Interactive terminal control, for the ASCII renderer.
// ─────────────────────────────────────────────────────────────────────────────

struct TerminalSize {
    int columns{80};
    int rows{24};
};

/// Current terminal dimensions, or a sane 80x24 default if they cannot be
/// determined (output redirected, no console attached).
[[nodiscard]] TerminalSize terminalSize() noexcept;

/// RAII guard: raw mode (no line buffering or echo), alternate screen buffer,
/// hidden cursor — and puts it all back, because a terminal left in raw mode
/// is user-hostile (`reset` is the only fix). The destructor does NOT run on
/// std::abort or a signal, which is why the renderer handles Ctrl-C itself.
class InteractiveSession {
public:
    InteractiveSession() noexcept;
    ~InteractiveSession() noexcept;

    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    /// False if the terminal could not be put into raw mode (output redirected,
    /// not a console). Bail out rather than render an animation into a pipe.
    [[nodiscard]] bool ok() const noexcept { return ok_; }

    /// Which check failed, when ok() is false. Reporting the specific gate
    /// turns "needs an interactive terminal" into a one-line diagnosis.
    [[nodiscard]] const char* failureReason() const noexcept { return reason_; }

private:
    bool        ok_{false};
    const char* reason_{"not attempted"};
};

/// Put stdout into binary mode, so the bytes we write are the bytes that arrive.
/// Windows stdout defaults to TEXT mode, which rewrites '\n' as "\r\n" —
/// corrupting byte-compared snapshots and expanding the renderer's deliberate
/// "\r\n" into "\r\r\n". No-op on POSIX.
void useBinaryStdout() noexcept;

/// Ask the console to interpret our output as UTF-8, and report whether it will.
/// Separate from enableAnsi(): a console can honour colour escapes and still
/// mangle multi-byte characters. The compiler's /utf-8 flag fixes the binary's
/// literals, not the console's decoding — that defaults to CP 437/1252, which
/// turns U+2580 into three Latin-1 characters. Called by InteractiveSession,
/// restored on the way out.
bool enableUtf8Output() noexcept;

/// Result of the most recent enableUtf8Output(). False until it has been called.
[[nodiscard]] bool supportsUtf8() noexcept;

/// Read one key without blocking. Returns 0 when nothing is pending. Plain
/// ASCII only: arrow/function keys are multi-byte and platform-specific, so
/// the renderer uses letter keys instead, which also works over ssh.
[[nodiscard]] int pollKey() noexcept;

/// Move the cursor to the top-left WITHOUT clearing: overwriting every cell
/// from a pre-composed buffer never flashes a blank partial frame.
[[nodiscard]] std::string_view cursorHome() noexcept;

/// Erase from the cursor to the end of the screen. Used once after a resize.
[[nodiscard]] std::string_view clearToEnd() noexcept;

/// Sleep for approximately \p duration, as precisely as the OS allows.
/// std::this_thread::sleep_for does not suffice on Windows: the default system
/// timer ticks every 15.625 ms and Sleep() rounds UP, so a nominal 30 fps loop
/// settles at 21 or 27 fps. Uses CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
/// (Windows 10 1803+, ~0.5 ms) rather than timeBeginPeriod(1), which raises
/// the timer rate machine-wide. Falls back to the portable sleep when the
/// flag is unavailable; POSIX nanosleep is already fine.
void sleepFor(std::chrono::nanoseconds duration) noexcept;

}  // namespace omma::console
