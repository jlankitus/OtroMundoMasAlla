#include "core/console.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

namespace con = omma::console;

TEST_CASE("colour accessors never return a null pointer", "[core][console]") {
    // Under ctest, stdout is a pipe rather than a terminal, so enableAnsi()
    // returns false and every accessor takes its disabled branch. That branch
    // must still yield a valid NUL-terminated pointer: a default-constructed
    // string_view has data() == nullptr, and printf("%s", nullptr) is
    // undefined behaviour that crashes on glibc and silently prints "(null)"
    // on MSVC. Exactly the sort of bug that only shows up in CI, on Linux, in
    // the error path.
    con::enableAnsi();

    const std::string_view codes[] = {
        con::reset(), con::bold(),  con::dim(),     con::red(),  con::green(),
        con::yellow(), con::blue(), con::magenta(), con::cyan(), con::white(),
    };

    for (const std::string_view code : codes) {
        REQUIRE(code.data() != nullptr);
        REQUIRE(std::strlen(code.data()) == code.size());
    }
}

TEST_CASE("colour is disabled when stdout is not a terminal", "[core][console]") {
    // ctest captures output through a pipe, so escape sequences would end up
    // in the log file rather than on a screen.
    REQUIRE_FALSE(con::enableAnsi());
    REQUIRE_FALSE(con::supportsAnsi());

    for (const std::string_view code : {con::reset(), con::bold(), con::green()}) {
        REQUIRE(code.empty());
    }
}

TEST_CASE("taking over the terminal never fails because of colour",
          "[core][console][regression]") {
    // The bug: a user with NO_COLOR=1 in their environment could not run the
    // interactive renderer AT ALL. It reported "needs an interactive terminal"
    // while sitting in an obviously interactive terminal, because the session
    // constructor bailed out as soon as colour was declined.
    //
    // https://no-color.org asks programs not to emit COLOUR. Cursor positioning,
    // line erasure and the alternate screen buffer are not colour. The correct
    // response to NO_COLOR is monochrome output, not refusal — and this program
    // already had a monochrome presentation.
    //
    // Asserted on the REASON rather than on ok(), so the test is meaningful
    // whether or not the machine running it happens to set NO_COLOR: under ctest
    // stdout is always a pipe, so the session legitimately fails — but it must
    // fail because of the pipe, never because of colour.
    const omma::console::InteractiveSession session;
    const std::string reason = session.failureReason();

    INFO("failure reason: " << reason);
    REQUIRE_FALSE(reason.empty());
    REQUIRE(reason.find("NO_COLOR") == std::string::npos);
    REQUIRE(reason.find("colour") == std::string::npos);
    REQUIRE(reason.find("color") == std::string::npos);

    SECTION("and the two capabilities are queryable independently") {
        // If these ever collapse back into one function, the bug returns.
        static_cast<void>(con::colourDeclinedByEnvironment());
        static_cast<void>(con::supportsVirtualTerminal());
        static_cast<void>(con::supportsAnsi());
        SUCCEED("colour and escape-processing are separate queries");
    }
}

TEST_CASE("enableAnsi is idempotent", "[core][console]") {
    const bool first = con::enableAnsi();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(con::enableAnsi() == first);
    }
    REQUIRE(con::supportsAnsi() == first);
}
