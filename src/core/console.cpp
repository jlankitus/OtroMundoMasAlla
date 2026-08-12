#include "core/console.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <conio.h>
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace omma::console {
namespace {

bool g_ansi = false;
bool g_asked = false;

bool stdoutIsTerminal() noexcept {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

/// Is an environment variable set to anything at all?
///
/// MSVC deprecates getenv in favour of _dupenv_s, which allocates. Since this
/// file is already the designated home for platform differences, the branch
/// lives here rather than being silenced with _CRT_SECURE_NO_WARNINGS — a
/// blanket macro that would also switch off the warnings we do want.
bool environmentVariableIsSet(const char* name) noexcept {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return false;
    }
    std::free(value);
    return true;
#else
    return std::getenv(name) != nullptr;
#endif
}

bool tryEnableVirtualTerminal() noexcept {
#if defined(_WIN32)
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) == 0) {
        return false;
    }
    // Already on (Windows Terminal does this for us) is success, not a no-op
    // we need to detect.
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        return true;
    }
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;   // every POSIX terminal worth the name handles this
#endif
}

}  // namespace

// TWO CAPABILITIES, NOT ONE. This distinction was a bug worth recording.
//
// "Can this terminal process escape sequences?" and "should this program use
// colour?" are different questions with different answers, and the first version
// answered both with enableAnsi().
//
// The consequence: a user with NO_COLOR=1 in their environment could not run the
// interactive renderer AT ALL. It reported "needs an interactive terminal" while
// sitting in an obviously interactive terminal, because the session constructor
// gave up as soon as colour was declined.
//
// https://no-color.org asks programs not to emit COLOUR. Cursor positioning,
// line erasure and the alternate screen buffer are not colour. Refusing to run
// is a misreading of the convention -- and the right response is to render in
// monochrome, which this program already knew how to do.
bool g_vtEnabled = false;
bool g_vtAsked = false;

bool enableVirtualTerminal() noexcept {
    if (g_vtAsked) {
        return g_vtEnabled;
    }
    g_vtAsked = true;
    // Escapes are meaningless down a pipe, but they are not harmful either; the
    // caller decides. What matters is whether the console will interpret them.
    g_vtEnabled = stdoutIsTerminal() && tryEnableVirtualTerminal();
    return g_vtEnabled;
}

bool supportsVirtualTerminal() noexcept { return g_vtEnabled; }

bool enableAnsi() noexcept {
    if (g_asked) {
        return g_ansi;
    }
    g_asked = true;

    const bool vt = enableVirtualTerminal();

    // Honour the community convention: https://no-color.org
    // This gates COLOUR only. It must never gate whether the program runs.
    if (environmentVariableIsSet("NO_COLOR")) {
        g_ansi = false;
        return g_ansi;
    }
    // Piping into a file or another program should produce clean text.
    g_ansi = vt;
    return g_ansi;
}

bool supportsAnsi() noexcept { return g_ansi; }

bool colourDeclinedByEnvironment() noexcept {
    return environmentVariableIsSet("NO_COLOR");
}

// The disabled case returns "" rather than a default-constructed string_view.
// A default-constructed view's data() is nullptr, and passing nullptr to
// printf's %s is undefined behaviour — it happens to print "(null)" on MSVC
// and crashes on glibc. Every accessor here is guaranteed to yield a valid
// NUL-terminated pointer.
#define OMMA_ANSI(name, code)                                            \
    std::string_view name() noexcept {                                   \
        return g_ansi ? std::string_view{code} : std::string_view{""};   \
    }

OMMA_ANSI(reset,   "\033[0m")
OMMA_ANSI(bold,    "\033[1m")
OMMA_ANSI(dim,     "\033[2m")
OMMA_ANSI(red,     "\033[31m")
OMMA_ANSI(green,   "\033[32m")
OMMA_ANSI(yellow,  "\033[33m")
OMMA_ANSI(blue,    "\033[34m")
OMMA_ANSI(magenta, "\033[35m")
OMMA_ANSI(cyan,    "\033[36m")
OMMA_ANSI(white,   "\033[37m")

#undef OMMA_ANSI

// ─────────────────────────────────────────────────────────────────────────────
// Interactive terminal
// ─────────────────────────────────────────────────────────────────────────────

std::string_view cursorHome() noexcept { return "\033[H"; }
std::string_view clearToEnd() noexcept { return "\033[0J"; }

TerminalSize terminalSize() noexcept {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info) != 0) {
        // srWindow is the visible viewport. dwSize is the scrollback buffer,
        // which on Windows is routinely 9000 rows tall -- using it would size
        // the canvas to the buffer rather than the window.
        const int columns = info.srWindow.Right - info.srWindow.Left + 1;
        const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (columns > 0 && rows > 0) {
            return TerminalSize{columns, rows};
        }
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        return TerminalSize{static_cast<int>(ws.ws_col), static_cast<int>(ws.ws_row)};
    }
#endif
    return TerminalSize{};
}

namespace {

#if !defined(_WIN32)
termios g_savedTermios{};
bool g_termiosSaved = false;
#else
unsigned int g_savedOutputCodePage = 0;
#endif

bool g_utf8 = false;

}  // namespace

void useBinaryStdout() noexcept {
#if defined(_WIN32)
    // _O_BINARY stops the CRT translating '\n' into "\r\n" on the way out.
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
#endif
}

bool enableUtf8Output() noexcept {
#if defined(_WIN32)
    // THE BUG THIS FIXES
    // The compiler's /utf-8 flag makes string literals UTF-8 in the binary. It
    // says nothing about how the console INTERPRETS the bytes we write. A
    // Windows console defaults to code page 437 or 1252 depending on locale, so
    // the three bytes of U+2580 arrive as three separate Latin-1 characters and
    // the entire display becomes "a-graveâ--EUR" repeated ten thousand times.
    //
    // This did not show up while the renderer emitted only ASCII. It appeared
    // the moment half blocks did, which is a good argument for testing the
    // presentation layer on a real console and not only through a pipe.
    if (g_savedOutputCodePage == 0) {
        g_savedOutputCodePage = GetConsoleOutputCP();
    }
    if (g_savedOutputCodePage == CP_UTF8) {
        g_utf8 = true;
        return true;
    }
    g_utf8 = SetConsoleOutputCP(CP_UTF8) != 0;
    return g_utf8;
#else
    // Any POSIX terminal in this century is UTF-8, and if it is not, the locale
    // says so rather than us guessing.
    g_utf8 = true;
    return true;
#endif
}

bool supportsUtf8() noexcept { return g_utf8; }

InteractiveSession::InteractiveSession() noexcept {
    // Deliberately does NOT consult NO_COLOR. Taking over the terminal needs
    // escape processing, not colour; see the note above enableVirtualTerminal().
    if (!stdoutIsTerminal()) {
        reason_ = "stdout is not a terminal (redirected to a file or a pipe)";
        return;
    }
    if (!enableVirtualTerminal()) {
        reason_ = "the console refused ANSI escape processing "
                  "(pre-1511 Windows, or a redirected handle)";
        return;
    }
    // Resolve the colour decision now so callers can query it, but ignore the
    // answer here.
    static_cast<void>(enableAnsi());

#if defined(_WIN32)
    // _getch() already bypasses line buffering and echo, so raw mode needs no
    // extra work here; ANSI output was enabled by enableAnsi() above.
    ok_ = true;
    reason_ = "";
#else
    if (tcgetattr(STDIN_FILENO, &g_savedTermios) != 0) {
        reason_ = "stdin is not a terminal (tcgetattr failed)";
        return;
    }
    g_termiosSaved = true;

    termios raw = g_savedTermios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;    // read() returns immediately...
    raw.c_cc[VTIME] = 0;   // ...with whatever is available, possibly nothing
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        reason_ = "could not put the terminal into raw mode (tcsetattr failed)";
        return;
    }
    ok_ = true;
    reason_ = "";
#endif

    if (ok_) {
        enableUtf8Output();
        // 1049 = alternate screen buffer, ?25l = hide cursor.
        std::fputs("\033[?1049h\033[?25l", stdout);
        std::fflush(stdout);
    }
}

InteractiveSession::~InteractiveSession() noexcept {
    if (!ok_) {
        return;
    }
    std::fputs("\033[?25h\033[?1049l", stdout);
    std::fflush(stdout);

#if defined(_WIN32)
    // Put the code page back. Leaving a shell in UTF-8 when it started in 437
    // silently breaks other programs the user runs afterwards, which is exactly
    // the class of rudeness the raw-mode guard exists to avoid.
    if (g_savedOutputCodePage != 0 && g_savedOutputCodePage != CP_UTF8) {
        SetConsoleOutputCP(g_savedOutputCodePage);
    }
#endif

#if !defined(_WIN32)
    if (g_termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios);
        g_termiosSaved = false;
    }
#endif
}

void sleepFor(std::chrono::nanoseconds duration) noexcept {
    if (duration <= std::chrono::nanoseconds::zero()) {
        return;
    }

#if defined(_WIN32)
    // Created once and reused. Creating a timer per frame would add a kernel
    // object allocation to every frame, which is exactly the sort of cost you
    // introduce while fixing a performance problem.
    static const HANDLE timer = [] {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_MANUAL_RESET
                                              | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        if (h == nullptr) {
            // Pre-1803 Windows rejects the high-resolution flag outright.
            // A normal timer is no better than Sleep(), so signal "no timer"
            // and let the portable path handle it.
            h = nullptr;
        }
        return h;
    }();

    if (timer != nullptr) {
        LARGE_INTEGER due{};
        // Negative means relative, in 100-nanosecond units.
        due.QuadPart = -(duration.count() / 100);
        if (due.QuadPart == 0) {
            due.QuadPart = -1;
        }
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif

    std::this_thread::sleep_for(duration);
}

int pollKey() noexcept {
#if defined(_WIN32)
    if (_kbhit() == 0) {
        return 0;
    }
    const int key = _getch();
    // 0 and 224 prefix an extended key (arrows, function keys). Consume the
    // second byte and report nothing, rather than letting the payload byte
    // masquerade as a letter -- the down arrow's second byte is 80, which is
    // 'P', and a stray 'P' doing something surprising is a maddening bug.
    if (key == 0 || key == 224) {
        static_cast<void>(_getch());
        return 0;
    }
    return key;
#else
    unsigned char byte = 0;
    const auto count = read(STDIN_FILENO, &byte, 1);
    if (count != 1) {
        return 0;
    }
    if (byte == 0x1b) {
        // An escape sequence. Drain it so its payload bytes are not mistaken
        // for keystrokes, and report nothing.
        unsigned char discard = 0;
        while (read(STDIN_FILENO, &discard, 1) == 1) {
        }
        return 0x1b;   // bare Esc, or an arrow key: either way, treat as Esc
    }
    return static_cast<int>(byte);
#endif
}

}  // namespace omma::console
