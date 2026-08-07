// ─────────────────────────────────────────────────────────────────────────────
// Camera — the world-to-screen projection, and nothing else.
//
// AN ISOMETRIC VIEW IS A CAMERA CHANGE, NOT A RENDERER CHANGE
// The top-down view is an orthographic projection that drops the z coordinate.
// Isometric is the same orthographic projection with a rotation applied first:
//
//     world  ──▶  yaw about z  ──▶  tilt about screen-x  ──▶  drop z
//
// Which reduces to two lines:
//
//     screenX = x·cos(az) + y·sin(az)
//     screenY = (−x·sin(az) + y·cos(az))·sin(el) + z·cos(el)
//
// At elevation 90° that collapses to (x, y) — the old top-down view, exactly.
// At elevation 0° it collapses to (x, z) — edge-on. Everything between is the
// tilted board.
//
// WHAT ROLLERCOASTER TYCOON ACTUALLY USES
// Not true isometric. True iso is elevation atan(1/√2) = 35.264°, where all
// three axes foreshorten equally — mathematically tidy, and it lands on
// fractional pixels. RCT uses 2:1 DIMETRIC: elevation 30°, tiles twice as wide
// as tall, every edge landing on whole pixels. sin(30°) = 0.5 exactly, so the
// vertical axis compresses by precisely one half.
//
// AND IT IS NOT DECORATION
// From directly overhead, a polar orbit projects to a straight line. A
// sun-synchronous satellite at 98° inclination — the workhorse of earth
// observation — renders as a vertical stroke through the planet and tells you
// nothing. Top-down actively fails the moment orbits stop being coplanar.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/vec3.hpp"

namespace omma::render {

/// Where a projected point landed.
///
/// THREE STATES, NOT TWO. An earlier version returned {0, 0, false} for
/// anything too far away to represent, which quietly broke line drawing: the
/// top-left corner is a perfectly valid coordinate, so every distant orbit grew
/// a spurious segment running to it. Off-screen and unrepresentable are
/// different things and need different answers.
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
    /// Elevation of the classic 2:1 dimetric view, in radians. 30 degrees.
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

    /// Multiply the scale. Clamped, so a stuck key cannot drive it to zero —
    /// dividing by which yields infinities that then silently fail every
    /// bounds check, and the display simply goes blank with no error.
    void zoomBy(double factor) noexcept;

    /// Set the scale so \p metres fits across the tighter axis.
    void frame(double metres) noexcept;

    /// Elevation above the reference plane, radians. Clamped to (0, pi/2]:
    /// exactly zero is edge-on, where the entire solar system collapses to a
    /// line and nothing is legible.
    void setElevation(double radians) noexcept;
    [[nodiscard]] double elevation() const noexcept { return elevation_; }

    /// Rotation about the vertical axis, radians. Wrapped, so spinning the view
    /// forever is fine.
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

    /// How far toward the viewer a world point sits, in metres, relative to the
    /// view centre. Larger is nearer.
    ///
    /// This is the depth buffer, in scalar form. It is what lets an orbit pass
    /// convincingly *behind* a planet instead of always over the top of it, and
    /// what a painter's-algorithm sort would order by.
    [[nodiscard]] double depthOf(const Vec3& world) const noexcept;

private:
    int    width_;
    int    height_;
    double unitAspect_{1.0};
    Vec3   centre_{};
    double metresPerRow_{1.0e10};
    double elevation_{kDimetricElevation};
    double azimuth_{0.0};

    // cos/sin of both angles, refreshed whenever either changes. They are used
    // several times per projected point and there are tens of thousands of
    // those per frame; recomputing the trig each time is the one avoidable cost
    // in this class.
    double cosElevation_{1.0};
    double sinElevation_{0.0};
    double cosAzimuth_{1.0};
    double sinAzimuth_{0.0};

    void refreshTrig() noexcept;
};

}  // namespace omma::render
