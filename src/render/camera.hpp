// ─────────────────────────────────────────────────────────────────────────────
// Camera — the world-to-screen projection, and nothing else.
//
// Kept separate from both the canvas and the renderer because it is the piece
// with the one non-obvious rule in it, and burying that rule inside a 400-line
// draw function is how it ends up being reimplemented slightly differently
// three more times.
//
// THE NON-OBVIOUS RULE: terminal cells are about twice as tall as they are
// wide. A projection that uses the same metres-per-cell on both axes makes
// every orbit look like an egg. So the horizontal scale is HALF the vertical
// one, which cancels the cell aspect and gives round circles.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/vec3.hpp"

namespace omma::render {

/// Where a projected point landed, and whether it is worth drawing.
struct ScreenPoint {
    int  x{0};
    int  y{0};
    bool onScreen{false};
};

class Camera {
public:
    Camera(int viewWidth, int viewHeight) noexcept;

    void setViewport(int width, int height) noexcept;

    /// World point the centre of the view is looking at, in metres.
    void setCentre(Vec3 centre) noexcept { centre_ = centre; }
    [[nodiscard]] Vec3 centre() const noexcept { return centre_; }

    /// Metres represented by one cell VERTICALLY. Horizontal is half this.
    void setScale(double metresPerRow) noexcept;
    [[nodiscard]] double metresPerRow() const noexcept { return metresPerRow_; }

    /// Multiply the scale, for zoom keys. Clamped to a sane range so a stuck
    /// key cannot drive the scale to zero or infinity and produce NaN
    /// coordinates that then silently swallow every draw call.
    void zoomBy(double factor) noexcept;

    /// Set the scale so that \p metres fits across the view's shorter axis.
    void frame(double metres) noexcept;

    /// Project a world position onto the grid. The z coordinate is dropped:
    /// this is a top-down view of the ecliptic plane, which is the right
    /// default because the solar system is very nearly flat.
    [[nodiscard]] ScreenPoint project(const Vec3& world) const noexcept;

    /// How wide the view currently is, in metres.
    [[nodiscard]] double viewWidthMetres() const noexcept;
    [[nodiscard]] double viewHeightMetres() const noexcept;

private:
    int    width_;
    int    height_;
    Vec3   centre_{};
    double metresPerRow_{1.0e10};
};

}  // namespace omma::render
