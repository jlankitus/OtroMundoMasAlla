#include "core/console.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

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

TEST_CASE("enableAnsi is idempotent", "[core][console]") {
    const bool first = con::enableAnsi();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(con::enableAnsi() == first);
    }
    REQUIRE(con::supportsAnsi() == first);
}
