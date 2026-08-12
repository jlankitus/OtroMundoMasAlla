// Camera — the world-to-screen projection, and nothing else.
//
// Orthographic, with a rotation applied first (yaw about z, tilt about
// screen-x, drop z):
//     screenX = x·cos(az) + y·sin(az)
//     screenY = (−x·sin(az) + y·cos(az))·sin(el) + z·cos(el)
// Elevation 90° reproduces the top-down chart. The default is 2:1 dimetric —
// 30°, the RollerCoaster Tycoon projection — which is what makes inclination
// visible at all. See docs/DESIGN.md §7.
#pragma once

#include "core/vec3.hpp"

namespace omma::render {

/// Where a projected point landed. THREE states, not two: off-screen and
/// unrepresentable are different answers, and conflating them once gave every
/// distant orbit a spurious segment to the top-left corner.
struct ScreenPoint {
    int  x{0};
    int  y{0};
    /// Inside the viewport.
    bool onScreen{false};
    /// Representable at all. False only for non-finite world coordinates.
    /// When true but onScreen is false, x and y are still meaningful and a
    /// clipped line through this point is correct to draw.
    bool valid{false};
};

class Camera {
public:
    /// The classic 2:1 dimetric elevation, radians: 30°, sin = 0.5 exactly.
    static constexpr double kDimetricElevation = 0.523598775598298873;

    /// Straight down. Reproduces a pure top-down chart.
    static constexpr double kTopDownElevation = 1.5707963267948966;

    /// \param viewWidth,viewHeight  in output units — character cells for a
    ///        character display, pixels for a half-block one.
    /// \param unitAspect  how much taller one output unit is than it is wide.
    ///        2.0 for character cells; 1.0 for the square pixels a half-block
    ///        canvas provides. Getting this wrong makes every orbit an egg.
    Camera(int viewWidth, int viewHeight, double unitAspect = 1.0) noexcept;

    void setViewport(int width, int height) noexcept;
    void setUnitAspect(double aspect) noexcept;

    void setCentre(Vec3 centre) noexcept { centre_ = centre; }
    [[nodiscard]] Vec3 centre() const noexcept { return centre_; }

    /// Metres per output unit VERTICALLY. Horizontal is this divided by the
    /// unit aspect.
    void setScale(double metresPerRow) noexcept;
    [[nodiscard]] double metresPerRow() const noexcept { return metresPerRow_; }

    /// Multiply the scale. Clamped: a stuck key must not drive it to zero
    /// (infinities silently blank the display).
    void zoomBy(double factor) noexcept;

    /// Set the scale so \p metres fits across the tighter axis.
    void frame(double metres) noexcept;

    /// Elevation above the reference plane, radians. Clamped to (0, pi/2]:
    /// exactly edge-on collapses the whole system to a line.
    void setElevation(double radians) noexcept;
    [[nodiscard]] double elevation() const noexcept { return elevation_; }

    /// Rotation about the vertical axis, radians. Wrapped.
    void setAzimuth(double radians) noexcept;
    [[nodiscard]] double azimuth() const noexcept { return azimuth_; }

    void tiltBy(double radians) noexcept { setElevation(elevation_ + radians); }
    void spinBy(double radians) noexcept { setAzimuth(azimuth_ + radians); }

    [[nodiscard]] ScreenPoint project(const Vec3& world) const noexcept;

    /// How much world the view currently spans.
    [[nodiscard]] double viewWidthMetres() const noexcept;
    [[nodiscard]] double viewHeightMetres() const noexcept;

    /// Unit vector from the scene toward the viewer — the axis the orthographic
    /// projection throws away.
    [[nodiscard]] Vec3 viewDirection() const noexcept;

    /// How far toward the viewer a world point sits, in metres, relative to
    /// the view centre; larger is nearer. The scalar depth buffer a
    /// painter's-algorithm sort orders by.
    [[nodiscard]] double depthOf(const Vec3& world) const noexcept;

private:
    int    width_;
    int    height_;
    double unitAspect_{1.0};
    Vec3   centre_{};
    double metresPerRow_{1.0e10};
    double elevation_{kDimetricElevation};
    double azimuth_{0.0};

    // cos/sin of both angles, refreshed whenever either changes: used several
    // times per projected point, tens of thousands of points per frame.
    double cosElevation_{1.0};
    double sinElevation_{0.0};
    double cosAzimuth_{1.0};
    double sinAzimuth_{0.0};

    void refreshTrig() noexcept;
};

}  // namespace omma::render
