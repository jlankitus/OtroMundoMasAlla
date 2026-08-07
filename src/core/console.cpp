#include "core/console.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <io.h>
#  include <windows.h>
#else
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

bool enableAnsi() noexcept {
    if (g_asked) {
        return g_ansi;
    }
    g_asked = true;

    // Honour the community convention: https://no-color.org
    if (environmentVariableIsSet("NO_COLOR")) {
        g_ansi = false;
        return g_ansi;
    }
    // Piping into a file or another program should produce clean text.
    if (!stdoutIsTerminal()) {
        g_ansi = false;
        return g_ansi;
    }
    g_ansi = tryEnableVirtualTerminal();
    return g_ansi;
}

bool supportsAnsi() noexcept { return g_ansi; }

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

}  // namespace omma::console
