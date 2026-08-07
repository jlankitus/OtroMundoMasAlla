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

#include <chrono>
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

/// RAII guard that puts the terminal into a state suitable for a full-screen
/// interactive app and — crucially — puts it back.
///
/// Raw mode disables line buffering and echo so keys arrive as they are
/// pressed. The alternate screen buffer means quitting restores whatever the
/// user had on screen before, rather than leaving a wall of orbital debris in
/// their scrollback.
///
/// This is RAII rather than a pair of functions because leaving a terminal in
/// raw mode with the cursor hidden is genuinely user-hostile: their shell
/// stops echoing what they type and the only fix is `reset`. A destructor runs
/// on the normal path, on an early return, and while an exception unwinds. It
/// does NOT run on std::abort or a signal, which is why the renderer also
/// handles Ctrl-C itself rather than letting the default handler take it.
class InteractiveSession {
public:
    InteractiveSession() noexcept;
    ~InteractiveSession() noexcept;

    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    /// False if the terminal could not be put into raw mode — output is
    /// redirected, or this is not a console. The caller should bail out rather
    /// than render an animation into a pipe.
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_{false};
};

/// Put stdout into binary mode, so the bytes we write are the bytes that arrive.
///
/// On Windows stdout defaults to TEXT mode, which silently rewrites every '\n'
/// as "\r\n". For a program emitting escape sequences that is corruption:
///
///   * a snapshot gains a stray '\r' on every row, so byte-comparing two frames
///     compares the wrong thing
///   * the live renderer deliberately writes "\r\n", which text mode expands to
///     "\r\r\n" -- harmless on most terminals, and one wasted byte per row per
///     frame forever
///
/// A renderer that controls the cursor needs to control the bytes. No-op on
/// POSIX, where nobody ever thought this was a good idea.
void useBinaryStdout() noexcept;

/// Ask the console to interpret our output as UTF-8, and report whether it will.
///
/// Separate from enableAnsi() because they are genuinely separate capabilities:
/// a console can honour colour escapes and still mangle multi-byte characters.
///
/// On Windows this is not optional. The compiler's /utf-8 flag makes string
/// literals UTF-8 in the binary; it says nothing about how the console decodes
/// the bytes. A console defaults to code page 437 or 1252, so the three bytes of
/// U+2580 arrive as three Latin-1 characters and a half-block display becomes
/// ten thousand copies of garbage. Called automatically by InteractiveSession,
/// and restored on the way out.
bool enableUtf8Output() noexcept;

/// Result of the most recent enableUtf8Output(). False until it has been called.
[[nodiscard]] bool supportsUtf8() noexcept;

/// Read one key without blocking. Returns 0 when nothing is pending.
///
/// Only reports plain ASCII. Arrow keys and function keys arrive as multi-byte
/// sequences that differ between platforms; the renderer uses letter keys
/// instead, which sidesteps that entirely and works over ssh.
[[nodiscard]] int pollKey() noexcept;

/// Move the cursor to the top-left WITHOUT clearing.
///
/// Clearing and redrawing produces visible flicker, because there is a moment
/// when the screen genuinely is blank. Overwriting every cell from a
/// pre-composed buffer never shows a partial frame. Same reason a game
/// double-buffers.
[[nodiscard]] std::string_view cursorHome() noexcept;

/// Erase from the cursor to the end of the screen. Used once after a resize.
[[nodiscard]] std::string_view clearToEnd() noexcept;

/// Sleep for approximately \p duration, as precisely as the OS allows.
///
/// WHY THIS EXISTS AND std::this_thread::sleep_for DOES NOT SUFFICE
/// Windows' default system timer fires every 15.625 ms, and Sleep() rounds UP
/// to the next tick. Ask for 28 ms and you get 31.25 ms, or 46.875 ms, never
/// 28. A frame loop that sleeps the remainder of a 33.3 ms budget therefore
/// produces frame times quantised to multiples of 15.6 ms, and a nominal
/// 30 fps target settles at 21 or 27 fps depending on phase. The renderer
/// looks slow; the renderer is fine.
///
/// The old fix is timeBeginPeriod(1), which raises the timer rate for the
/// WHOLE MACHINE — every process, plus a measurable battery cost. Windows 10
/// 1803 added CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, which gives roughly
/// half-millisecond granularity to just this wait. We use that, and fall back
/// to the portable sleep when it is unavailable.
///
/// On POSIX, nanosleep is already fine and this forwards straight to it.
void sleepFor(std::chrono::nanoseconds duration) noexcept;

}  // namespace omma::console
