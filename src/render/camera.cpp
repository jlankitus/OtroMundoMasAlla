#include "render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace omma::render {
namespace {

/// A terminal cell is roughly twice as tall as it is wide, so one cell of
/// horizontal travel covers half as much world as one cell of vertical travel.
constexpr double kCellAspect = 2.0;

/// Zoomed all the way in, one row is a metre; all the way out, a thousand au.
/// The clamp exists so a held key cannot reach zero (division by which yields
/// infinities that quietly fail every bounds check) or overflow to NaN.
constexpr double kMinMetresPerRow = 1.0;
constexpr double kMaxMetresPerRow = 1.0e15;

}  // namespace

Camera::Camera(int viewWidth, int viewHeight) noexcept
    : width_{std::max(1, viewWidth)}, height_{std::max(1, viewHeight)} {}

void Camera::setViewport(int width, int height) noexcept {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
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
    const double byHeight = metres / static_cast<double>(height_);
    const double byWidth = metres * kCellAspect / static_cast<double>(width_);
    setScale(std::max(byHeight, byWidth));
}

ScreenPoint Camera::project(const Vec3& world) const noexcept {
    const double metresPerColumn = metresPerRow_ / kCellAspect;

    const double dx = (world.x - centre_.x) / metresPerColumn;
    // Screen rows increase downward; world +y is "up" on a chart, so this is
    // the one place the sign has to flip. Forgetting it produces a solar
    // system that orbits backwards, which is surprisingly easy to not notice.
    const double dy = -(world.y - centre_.y) / metresPerRow_;

    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return ScreenPoint{0, 0, false};
    }

    // Guard the cast: a body far outside the view can produce a value beyond
    // int range, and an out-of-range double-to-int conversion is undefined
    // behaviour rather than a large integer.
    constexpr double kLimit = 1.0e6;
    if (std::abs(dx) > kLimit || std::abs(dy) > kLimit) {
        return ScreenPoint{0, 0, false};
    }

    const int x = static_cast<int>(std::lround(dx)) + width_ / 2;
    const int y = static_cast<int>(std::lround(dy)) + height_ / 2;
    return ScreenPoint{x, y, x >= 0 && y >= 0 && x < width_ && y < height_};
}

double Camera::viewWidthMetres() const noexcept {
    return static_cast<double>(width_) * metresPerRow_ / kCellAspect;
}

double Camera::viewHeightMetres() const noexcept {
    return static_cast<double>(height_) * metresPerRow_;
}

}  // namespace omma::render
