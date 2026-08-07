#include "render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace omma::render {
namespace {

/// Zoomed all the way in one output unit is a metre; all the way out, a
/// thousand au. The clamp exists so a held key cannot reach zero or overflow.
constexpr double kMinMetresPerRow = 1.0;
constexpr double kMaxMetresPerRow = 1.0e15;

/// Elevation is clamped away from zero. Exactly edge-on collapses the whole
/// system onto one line, which is a legitimate projection and a useless view.
constexpr double kMinElevation = 0.02;
constexpr double kMaxElevation = 1.5707963267948966;   // pi/2

constexpr double kTwoPi = 6.283185307179586476925287;

}  // namespace

Camera::Camera(int viewWidth, int viewHeight, double unitAspect) noexcept
    : width_{std::max(1, viewWidth)},
      height_{std::max(1, viewHeight)},
      unitAspect_{unitAspect > 0.0 ? unitAspect : 1.0} {
    refreshTrig();
}

void Camera::refreshTrig() noexcept {
    cosElevation_ = std::cos(elevation_);
    sinElevation_ = std::sin(elevation_);
    cosAzimuth_ = std::cos(azimuth_);
    sinAzimuth_ = std::sin(azimuth_);
}

void Camera::setViewport(int width, int height) noexcept {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void Camera::setUnitAspect(double aspect) noexcept {
    if (aspect > 0.0 && std::isfinite(aspect)) {
        unitAspect_ = aspect;
    }
}

void Camera::setScale(double metresPerRow) noexcept {
    if (!std::isfinite(metresPerRow)) {
        return;
    }
    metresPerRow_ = std::clamp(metresPerRow, kMinMetresPerRow, kMaxMetresPerRow);
}

void Camera::zoomBy(double factor) noexcept {
    setScale(metresPerRow_ * factor);
}

void Camera::frame(double metres) noexcept {
    // Fit across whichever axis is tighter, so nothing gets cropped.
    //
    // Note the elevation term. A tilted view compresses the vertical extent by
    // sin(elevation), so at 30 degrees the same span needs only half the rows —
    // meaning framing the system and then tilting would leave the view zoomed
    // too far out. Accounting for it here keeps "fit the solar system" meaning
    // the same thing at every tilt.
    const double byHeight = metres * std::max(sinElevation_, 0.2)
                          / static_cast<double>(height_);
    const double byWidth = metres * unitAspect_ / static_cast<double>(width_);
    setScale(std::max(byHeight, byWidth));
}

void Camera::setElevation(double radians) noexcept {
    if (!std::isfinite(radians)) {
        return;
    }
    elevation_ = std::clamp(radians, kMinElevation, kMaxElevation);
    refreshTrig();
}

void Camera::setAzimuth(double radians) noexcept {
    if (!std::isfinite(radians)) {
        return;
    }
    azimuth_ = std::fmod(radians, kTwoPi);
    if (azimuth_ < 0.0) {
        azimuth_ += kTwoPi;
    }
    refreshTrig();
}

ScreenPoint Camera::project(const Vec3& world) const noexcept {
    const double x = world.x - centre_.x;
    const double y = world.y - centre_.y;
    const double z = world.z - centre_.z;

    // Yaw about the vertical axis, then tilt about the screen's horizontal
    // axis, then drop the depth. See the header for the derivation.
    const double eastward = x * cosAzimuth_ + y * sinAzimuth_;
    const double northward = -x * sinAzimuth_ + y * cosAzimuth_;

    const double screenX = eastward;
    const double screenY = northward * sinElevation_ + z * cosElevation_;

    const double metresPerColumn = metresPerRow_ / unitAspect_;
    const double dx = screenX / metresPerColumn;
    // Screen rows increase downward; a chart puts +y at the top. This is the
    // one place a sign has to flip, and forgetting it produces a solar system
    // that orbits backwards.
    const double dy = -screenY / metresPerRow_;

    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return ScreenPoint{0, 0, /*onScreen=*/false, /*valid=*/false};
    }

    // Guard the cast: an out-of-range double-to-int conversion is undefined
    // behaviour, not a large integer.
    //
    // CLAMP, do not discard. The direction has to survive, because a line whose
    // far endpoint is a million units off-screen still crosses the viewport and
    // still has to be drawn. Returning a fixed sentinel here is what made
    // distant orbits render as dashes with stray segments to the corner.
    constexpr double kLimit = 1.0e6;
    const double clampedX = std::clamp(dx, -kLimit, kLimit);
    const double clampedY = std::clamp(dy, -kLimit, kLimit);

    const int px = static_cast<int>(std::lround(clampedX)) + width_ / 2;
    const int py = static_cast<int>(std::lround(clampedY)) + height_ / 2;
    return ScreenPoint{px, py,
                       px >= 0 && py >= 0 && px < width_ && py < height_,
                       /*valid=*/true};
}

double Camera::viewWidthMetres() const noexcept {
    return static_cast<double>(width_) * metresPerRow_ / unitAspect_;
}

double Camera::viewHeightMetres() const noexcept {
    // The visible world span vertically, undoing the tilt compression, so the
    // HUD reports what you can actually see rather than a raw row count.
    return static_cast<double>(height_) * metresPerRow_ / std::max(sinElevation_, 0.2);
}

Vec3 Camera::viewDirection() const noexcept {
    // Unit vector from the scene toward the viewer.
    //
    // Derived by completing the rotation rather than guessing. The projection
    // keeps two of the three rotated axes and discards the third; that
    // discarded axis IS the view direction. With
    //
    //     northward = -x·sin(az) + y·cos(az)
    //     screenY   =  northward·sin(el) + z·cos(el)
    //
    // the component orthogonal to both screenX and screenY is
    //
    //     depth = z·sin(el) - northward·cos(el)
    //           = x·sin(az)·cos(el) - y·cos(az)·cos(el) + z·sin(el)
    //
    // so the coefficients of x, y and z are the direction's components. It is
    // already unit length, being a row of a rotation matrix.
    //
    // Sanity checks, both of which are asserted in the tests: at elevation 90
    // this is (0, 0, 1) — looking straight down from ecliptic north. At
    // elevation 0 with azimuth 0 it is (0, -1, 0) — edge-on from the -y side.
    return Vec3{sinAzimuth_ * cosElevation_,
                -cosAzimuth_ * cosElevation_,
                sinElevation_};
}

double Camera::depthOf(const Vec3& world) const noexcept {
    const Vec3 relative{world.x - centre_.x, world.y - centre_.y, world.z - centre_.z};
    return dot(relative, viewDirection());
}

}  // namespace omma::render
