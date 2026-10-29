#pragma once
/// @file glu32_quadric.hpp
/// @brief GLU 1.2 quadrics (gluSphere, gluCylinder, gluDisk, gluPartialDisk):
///        the GL command sequence each draws, as a pure function of the
///        quadric's state and the arguments.
///
/// The sequence is Microsoft glu32's, measured through GL_FEEDBACK on
/// winoracle (tools/oracle_probe/gl11_state_probe64.expected, glu-fb-* lines:
/// vertex positions, normals and texture coordinates of every primitive for
/// each draw style, normal mode and orientation; glu-quadric-* for the
/// errors, the 239-slice cap and the current values a draw leaves), with the
/// shapes as the GLU 1.2 specification describes them: vertex i of a slice
/// lies at angle 2*pi*i/slices from +y towards +x (x = r sin, y = r cos),
/// texture s runs 1 - i/slices (s = 0.75 at +x, the measured value; the GLU
/// 1.2 text says 0.25), sphere t runs 1 at +z to 0 at -z, cylinder t 0 at
/// z = 0 to 1 at z = height, disk (s, t) = (0.5, 0.5) + r / outer / 2 *
/// (sin, cos). Written from that description and the measurements only.
///
/// Where glu32 sends normals of uninitialised memory (GLU_POINT with
/// GLU_FLAT, and the silhouette of a GLU_FLAT cylinder: the two builds of the
/// probe print different stack contents), Vitreo sends the vertex
/// (GLU_SMOOTH) normals.

#include <vitreo/common.hpp>

namespace vitreo::winapi::glu {

inline constexpr u32 GLU_SMOOTH = 100000;
inline constexpr u32 GLU_FLAT = 100001;
inline constexpr u32 GLU_NONE = 100002;
inline constexpr u32 GLU_POINT = 100010;
inline constexpr u32 GLU_LINE = 100011;
inline constexpr u32 GLU_FILL = 100012;
inline constexpr u32 GLU_SILHOUETTE = 100013;
inline constexpr u32 GLU_OUTSIDE = 100020;
inline constexpr u32 GLU_INSIDE = 100021;
inline constexpr u32 GLU_ERROR = 100103;
inline constexpr u32 GLU_INVALID_ENUM = 100900;
inline constexpr u32 GLU_INVALID_VALUE = 100901;
inline constexpr u32 GLU_OUT_OF_MEMORY = 100902;

/// glu-quadric-disk-300-slices-points (478 = 2 x 239), -sphere-300-2-points,
/// -sphere-4-300-points (960 = 240 x 4: sphere stacks are capped too).
inline constexpr i32 kQuadricCacheSize = 240;

/// gluNewQuadric's defaults (GLU 1.2: GLU_FILL, GLU_SMOOTH, GLU_OUTSIDE,
/// no texture coordinates, no callback).
struct QuadricState {
    u32 draw_style{GLU_FILL};
    u32 normals{GLU_SMOOTH};
    u32 orientation{GLU_OUTSIDE};
    bool texture{false};
    GuestAddr error_callback{0};
};

/// Where a quadric's GL calls go (production: glu32's GL issue through
/// opengl32's display-list seam; tests: a recorder).
class QuadricSink {
public:
    virtual ~QuadricSink() = default;
    virtual void begin(u32 mode) = 0;
    virtual void end() = 0;
    virtual void normal(float x, float y, float z) = 0;
    virtual void texcoord(float s, float t) = 0;
    virtual void vertex(float x, float y, float z) = 0;
};

/// Each draw returns 0, or the GLU error for the quadric's callback
/// (GLU_INVALID_VALUE: nothing was drawn). The checks are glu32's
/// (glu-quadric-err-*): slices < 2, stacks / loops < 1, a negative radius or
/// height, outer < inner; a cylinder of zero slant length (equal radii and
/// height 0) and a disk with outer radius <= 0 are invalid as well.
u32 draw_sphere(const QuadricState& q, double radius, i32 slices, i32 stacks, QuadricSink& out);
u32 draw_cylinder(const QuadricState& q, double base_radius, double top_radius, double height, i32 slices,
                  i32 stacks, QuadricSink& out);
u32 draw_partial_disk(const QuadricState& q, double inner_radius, double outer_radius, i32 slices, i32 loops,
                      double start_angle, double sweep_angle, QuadricSink& out);
/// gluDisk is gluPartialDisk(0, 360).
inline u32 draw_disk(const QuadricState& q, double inner_radius, double outer_radius, i32 slices, i32 loops,
                     QuadricSink& out) {
    return draw_partial_disk(q, inner_radius, outer_radius, slices, loops, 0.0, 360.0, out);
}

}  // namespace vitreo::winapi::glu
