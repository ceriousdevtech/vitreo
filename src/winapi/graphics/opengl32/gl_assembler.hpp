#pragma once
/// @file gl_assembler.hpp
/// @brief OpenGL 1.1 primitive assembly (spec 2.6.1): the ten glBegin modes
///        become point, line and triangle lists the backend draws.
///
/// Every emitted primitive lists GL's provoking vertex FIRST (spec 2.13.7,
/// Table 2.9: the last vertex of each point, segment, triangle and
/// quadrilateral, vertex 1 of a polygon), because the vertex Metal's
/// [[flat]] attributes take is the first. Triangles are rotations of the
/// spec's vertex order, so their winding is unchanged. Segments are emitted
/// (provoking, other), so a GL segment runs from indices[2k + 1] to
/// indices[2k]; Task 9's CPU line expansion (diamond-exit rule, stipple)
/// must read them in that order.
///
/// Quadrilaterals and polygons are split into triangles along diagonals
/// through the provoking vertex. Polygon edge flags (spec 2.6.2) come out as
/// one 3-bit mask per triangle: bit k is set when the edge from emitted corner
/// k to corner (k + 1) mod 3 is a boundary edge. Diagonals the split adds are
/// never boundary; strips and fans ignore the flags (all their edges are
/// boundary). glPolygonMode(GL_LINE / GL_POINT) reads polygons() and
/// GlPrimitiveExpander turns the edges and vertices into window-aligned
/// triangles (gl_backend_draw.cpp).

#include <vitreo/common.hpp>

#include <array>
#include <initializer_list>
#include <span>
#include <vector>

namespace vitreo::winapi::gl {

/// One assembled vertex: object coordinates and the current values at its
/// glVertex (spec 2.7), plus what the CPU line and point expansion
/// (GlPrimitiveExpander) gives each corner: the stipple counter
/// (line_distance), the window-pixel displacement of the corner
/// (window_offset) and the polygon-offset terms (depth_offset; see
/// FFPipelineState::window_offset / depth_offset). 80 bytes, the stride of
/// the streaming vertex buffer.
struct GlBatchVertex {
    float pos[4];
    float color[4];
    float normal[3];
    float tex[4];
    float line_distance;
    float window_offset[2];
    float depth_offset[2];
};
static_assert(sizeof(GlBatchVertex) == 80);

enum class GlPrimitiveClass : u8 { None, Points, Lines, Triangles };

/// The list a glBegin mode assembles into; None for a token that is not one
/// of the ten primitive modes.
[[nodiscard]] GlPrimitiveClass gl_primitive_class(u32 mode) noexcept;

/// Edge-mask bits of an emitted triangle.
inline constexpr u8 kGlEdge01 = 1u, kGlEdge12 = 2u, kGlEdge20 = 4u;

class GlAssembler {
public:
    /// Assembles @p vertices as primitive @p mode. @p edge_flags is one flag
    /// per vertex (spec 2.6.2), or empty for all TRUE. The output replaces the
    /// previous one; indices index @p vertices. Incomplete trailing primitives
    /// are dropped (spec 2.6.1).
    void emit(u32 mode, std::span<const GlBatchVertex> vertices, std::span<const u8> edge_flags = {});

    [[nodiscard]] GlPrimitiveClass primitive_class() const noexcept { return m_class; }
    [[nodiscard]] std::span<const u32> indices() const noexcept { return m_indices; }
    /// One mask per triangle (empty for points and lines).
    [[nodiscard]] std::span<const u8> triangle_edges() const noexcept { return m_edges; }

    /// The GL polygons the triangles came from (spec 2.6.1: each triangle of
    /// a strip or fan, each quadrilateral, the polygon), for glPolygonMode,
    /// which needs a polygon's boundary in its own order, its facing and its
    /// vertices once each (spec 3.5.4).
    struct Polygon {
        u32 first_vertex;     ///< into polygon_vertices() / polygon_boundary()
        u32 vertex_count;
        u32 first_triangle;   ///< into indices() / 3
        u32 triangle_count;
    };
    [[nodiscard]] std::span<const Polygon> polygons() const noexcept { return m_polygons; }
    /// Each polygon's vertices (indices into the emitted vertices) in the
    /// spec's boundary order, which is the order that gives its winding.
    [[nodiscard]] std::span<const u32> polygon_vertices() const noexcept { return m_polygon_vertices; }
    /// Per polygon vertex: whether the edge it begins is a boundary edge
    /// (spec 2.6.2).
    [[nodiscard]] std::span<const u8> polygon_boundary() const noexcept { return m_polygon_boundary; }

private:
    void triangle(u32 a, u32 b, u32 c, u8 edges);
    void segment(u32 provoking, u32 other);
    void polygon(std::initializer_list<u32> vertices, std::initializer_list<u8> boundary, u32 triangles);

    GlPrimitiveClass m_class{GlPrimitiveClass::None};
    std::vector<u32> m_indices;
    std::vector<u8> m_edges;
    std::vector<Polygon> m_polygons;
    std::vector<u32> m_polygon_vertices;
    std::vector<u8> m_polygon_boundary;
};

/// GL window coordinates of a vertex (spec 2.10, 2.10.1): clip = P * MV *
/// object (GL's clip volume, -w <= z <= w), then the perspective divide and
/// the viewport / depth-range map. Double precision.
struct GlWindowMap {
    std::array<double, 16> clip{};        ///< P * MV, column-major (m[c * 4 + r])
    std::array<double, 16> eye{};         ///< MV (user clip planes are eye space)
    double x{0}, y{0}, width{1}, height{1};
    double depth_near{0}, depth_far{1};
    /// Enabled user clip planes (spec 2.11), eye coordinates.
    std::vector<std::array<double, 4>> clip_planes;

    [[nodiscard]] std::array<double, 4> to_clip(const float pos[4]) const noexcept;
    [[nodiscard]] std::array<double, 4> to_eye(const float pos[4]) const noexcept;
    /// x_w, y_w, z_w of clip coordinates @p c (w != 0).
    [[nodiscard]] std::array<double, 3> to_window(const std::array<double, 4>& c) const noexcept;
};

/// The CPU expansion of GL points and line segments into window-aligned
/// triangles (Design 6), which the backend then rasterises exactly as the
/// spec's aliased rules produce fragments:
/// - Points (spec 3.3): a point outside the clip volume is discarded; the
///   rest become a square of the rounded size w (>= 1) around (floor(x_w) +
///   1/2, floor(y_w) + 1/2) for odd w and (floor(x_w + 1/2), floor(y_w +
///   1/2)) for even w, so exactly the w x w fragment centres inside it are
///   produced. Every corner keeps the vertex's object position and data; the
///   displacement to the corner is its window_offset.
/// - Segments (spec 3.4.1-3.4.2): clipped against the clip volume and the
///   user planes in clip space (2.11), then rasterised by the diamond-exit
///   rule: the fragment columns (rows for a y-major segment) it produces are
///   found exactly on the CPU, and a parallelogram whose ends lie on the
///   outer pixel edges of the first and last column, and whose long edges
///   are the segment offset by +-w/2 in the minor direction, covers exactly
///   the fragment centres of the w-high column at each of them (the column
///   rule of wide lines). Corners lie on the segment's line in clip space
///   (their object position is the homogeneous interpolation), so data are
///   interpolated as eq. 3.2 does; window_offset moves them +-w/2 in the
///   minor direction. line_distance is the stipple counter s at the corner's
///   column edge (FFPipelineState::line_stipple), continuing from the
///   caller's counter.
/// - Facing: each quad is wound so the backend reports it front- or
///   back-facing as asked (glPolygonMode edges and vertices take their
///   polygon's facing; points and lines are front). Nothing the backend does
///   for these quads reads it today: they are never culled, GL 1.1 stencil
///   has one state for both faces, and their colours arrive lit on the CPU
///   (front or back by the polygon's CPU facing). It keeps the facing
///   consistent for any later face-dependent state.
class GlPrimitiveExpander {
public:
    GlWindowMap map;
    float point_size{1.0f};          ///< GL_POINT_SIZE (rounded here)
    float line_width{1.0f};          ///< GL_LINE_WIDTH (rounded here)
    bool front_is_ccw{true};         ///< glFrontFace(GL_CCW)

    void clear() noexcept {
        vertices.clear();
        indices.clear();
    }
    /// A point at @p v carrying @p data's colour, normal and texture
    /// coordinates (v itself, or a flat-shaded polygon's provoking vertex).
    void point(const GlBatchVertex& v, const GlBatchVertex& data, bool front, const float depth_offset[2]);
    /// The segment from @p a to @p b (spec order). @p flat_data: the
    /// provoking vertex's data for flat shading, or null. @p counter is the
    /// stipple counter s, advanced by the fragments produced.
    void segment(const GlBatchVertex& a, const GlBatchVertex& b, const GlBatchVertex* flat_data, bool front,
                 float& counter, const float depth_offset[2]);

    std::vector<GlBatchVertex> vertices;
    std::vector<u32> indices;        ///< triangle list

private:
    void quad(const std::array<GlBatchVertex, 4>& corners, const std::array<std::array<double, 2>, 4>& window,
              bool front);
};

/// Rounds a GL point size or line width for aliased rasterization (spec 3.3,
/// 3.4.2): to the nearest integer, at least 1.
[[nodiscard]] u32 gl_aliased_width(float width) noexcept;

}  // namespace vitreo::winapi::gl
