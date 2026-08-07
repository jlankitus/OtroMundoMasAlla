#include "core/version.hpp"

#include <string>

namespace omma {

std::string_view versionBanner() noexcept {
    // Built once at first call, and the storage outlives every caller.
    static const std::string banner = std::string(kProjectName) + " " + std::string(kVersion);
    return banner;
}

}  // namespace omma
