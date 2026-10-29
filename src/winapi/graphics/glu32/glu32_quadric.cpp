// SPDX-License-Identifier: LGPL-2.1-only
/// @file glu32_quadric.cpp
/// @brief GLU 1.2 quadric tessellation (glu32_quadric.hpp).
///
/// Each shape is a grid: a vertex per (around, along) index pair, with its
/// position, texture coordinates and normal computed from the GLU 1.2 shape
/// description. The order in which grid points become GL primitives, and the
/// normal each one carries, is what Microsoft glu32 sends, as recovered from
/// its GL_FEEDBACK output (glu-fb-* in gl11_state_probe64.expected). The rules
/// per draw style are written next to each emitter.

#include "glu32_quadric.hpp"

#include <array>
#include <cmath>

// glu32 rounds every product (no fused multiply-add), as in glu32.cpp: the
// disk's texture coordinates scale * sin(a) + 0.5 among them.
#pragma clang fp contract(off)

namespace vitreo::winapi::glu {

namespace {

constexpr u32 kPoints = 0x0000;
constexpr u32 kLineStrip = 0x0003;
constexpr u32 kTriangleFan = 0x0006;
constexpr u32 kQuadStrip = 0x0008;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kHalfTurnDegrees = 180.0;

using Vec3 = std::array<double, 3>;

/// GL calls for one quadric: texture coordinates only when the quadric
/// generates them, normals only when its normal mode is not GLU_NONE.
class Out {
public:
    Out(const QuadricState& q, QuadricSink& sink) : m_q(q), m_sink(sink) {}
    [[nodiscard]] bool normals() const noexcept { return m_q.normals != GLU_NONE; }
    [[nodiscard]] bool flat() const noexcept { return m_q.normals == GLU_FLAT; }
    [[nodiscard]] bool smooth() const noexcept { return m_q.normals == GLU_SMOOTH; }
    [[nodiscard]] bool outward() const noexcept { return m_q.orientation == GLU_OUTSIDE; }
    void begin(u32 mode) { m_sink.begin(mode); }
    void end() { m_sink.end(); }
    void normal(const Vec3& n) {
        m_sink.normal(static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]));
    }
    /// A vertex with its texture coordinates (s, t).
    void vertex(const Vec3& p, double s, double t) {
        if (m_q.texture) m_sink.texcoord(static_cast<float>(s), static_cast<float>(t));
        m_sink.vertex(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    }

private:
    const QuadricState& m_q;
    QuadricSink& m_sink;
};

/// Slice and stack counts are capped at 239 (glu-quadric-disk-300-slices-points,
/// -sphere-300-2-points, -sphere-4-300-points).
i32 capped(i32 n) noexcept { return n >= kQuadricCacheSize ? kQuadricCacheSize - 1 : n; }

/// Direction of slice boundary @p i of @p n around the z axis, measured from
/// +y towards +x; i == n is i == 0 again. @p half_back shifts it half a slice
/// back (the middle of the slice before boundary i: a face normal).
struct Around {
    i32 n;
    [[nodiscard]] double angle(double i) const noexcept { return kTwoPi * i / n; }
    [[nodiscard]] double sin_of(i32 i, bool half_back = false) const noexcept {
        return half_back ? std::sin(angle(i - 0.5)) : (i == n ? std::sin(0.0) : std::sin(angle(i)));
    }
    [[nodiscard]] double cos_of(i32 i, bool half_back = false) const noexcept {
        return half_back ? std::cos(angle(i - 0.5)) : (i == n ? std::cos(0.0) : std::cos(angle(i)));
    }
    /// GLU 1.2: s runs 1 - i/n (Microsoft glu32: s = 0.75 at +x, glu-fb-*).
    [[nodiscard]] double s_of(i32 i) const noexcept { return 1.0 - static_cast<double>(i) / n; }
};

// ---- Sphere ----------------------------------------------------------------------------

/// Point (i, j) of a sphere: slice boundary i, latitude j = 0 (+z pole) .. stacks
/// (-z pole), angle pi j / stacks from +z.
struct SphereGrid {
    Around around;
    i32 stacks;
    double radius;
    double sign;   ///< +1 outward normals, -1 inward

    [[nodiscard]] double polar(double j) const noexcept { return 3.14159265358979323846 * j / stacks; }
    [[nodiscard]] Vec3 point(i32 i, i32 j) const noexcept {
        // The poles are single points: their ring radius is exactly 0.
        const double ring = (j == 0 || j == stacks) ? 0.0 : radius * std::sin(polar(j));
        return {ring * around.sin_of(i), ring * around.cos_of(i), radius * std::cos(polar(j))};
    }
    [[nodiscard]] double t_of(i32 j) const noexcept { return 1.0 - static_cast<double>(j) / stacks; }
    /// The normal at a grid point.
    [[nodiscard]] Vec3 vertex_normal(i32 i, i32 j) const noexcept {
        const double st = std::sin(polar(j)), ct = std::cos(polar(j));
        return {sign * around.sin_of(i) * st, sign * around.cos_of(i) * st, sign * ct};
    }
    /// A normal for GLU_FLAT: the direction half a slice before boundary @p i
    /// (when @p around_half) and at polar angle @p polar_index (fractional).
    [[nodiscard]] Vec3 facet_normal(i32 i, bool around_half, double polar_index) const noexcept {
        const double st = std::sin(polar(polar_index)), ct = std::cos(polar(polar_index));
        return {sign * around.sin_of(i, around_half) * st, sign * around.cos_of(i, around_half) * st, sign * ct};
    }
};

/// GLU_FILL band between latitudes j and j + 1 as a quad strip. Outward: the
/// lower-index point comes second (point (i, j + 1) then (i, j)); inward the
/// other way round. GLU_SMOOTH gives each vertex its own normal; GLU_FLAT
/// gives only the second vertex of each pair a face normal (the slice half a
/// step back, the band's middle latitude), so the first inherits the previous
/// one (glu-fb-fill-flat-sphere-*).
void sphere_band(Out& out, const SphereGrid& g, i32 j) {
    const i32 first_row = out.outward() ? j + 1 : j;
    const i32 second_row = out.outward() ? j : j + 1;
    out.begin(kQuadStrip);
    for (i32 i = 0; i <= g.around.n; ++i) {
        if (out.smooth()) out.normal(g.vertex_normal(i, first_row));
        out.vertex(g.point(i, first_row), g.around.s_of(i), g.t_of(first_row));
        if (out.smooth()) out.normal(g.vertex_normal(i, second_row));
        if (out.flat()) out.normal(g.facet_normal(i, true, j + 0.5));
        out.vertex(g.point(i, second_row), g.around.s_of(i), g.t_of(second_row));
    }
    out.end();
}

/// GLU_FILL cap without texture coordinates: a fan from a pole over the
/// nearest latitude. @p top: +z (latitude 1) or -z (latitude stacks - 1). The
/// rim runs i = slices .. 0 for the top seen from outside and i = 0 .. slices
/// for the bottom, reversed for inward normals. GLU_SMOOTH sets the pole's
/// normal before the fan and each rim vertex's own; GLU_FLAT gives the rim
/// face normals, skipping the first rim vertex of a descending rim
/// (glu-fb-fill-none-notex-sphere-*).
void sphere_cap(Out& out, const SphereGrid& g, bool top) {
    const i32 rim = top ? 1 : g.stacks - 1;
    const double band_middle = top ? 0.5 : g.stacks - 0.5;
    if (out.smooth()) out.normal({0.0, 0.0, top ? g.sign : -g.sign});
    out.begin(kTriangleFan);
    out.vertex({0.0, 0.0, top ? g.radius : -g.radius}, 0.0, 0.0);
    const bool descending = top == out.outward();
    for (i32 k = 0; k <= g.around.n; ++k) {
        const i32 i = descending ? g.around.n - k : k;
        if (out.smooth()) out.normal(g.vertex_normal(i, rim));
        if (out.flat()) {
            if (descending && i != g.around.n) out.normal(g.facet_normal(i + 1, true, band_middle));
            if (!descending) out.normal(g.facet_normal(i, true, band_middle));
        }
        out.vertex(g.point(i, rim), 0.0, 0.0);
    }
    out.end();
}

}  // namespace

u32 draw_sphere(const QuadricState& q, double radius, i32 slices, i32 stacks, QuadricSink& sink) {
    slices = capped(slices);
    stacks = capped(stacks);
    if (slices < 2 || stacks < 1 || radius < 0.0) return GLU_INVALID_VALUE;
    Out out(q, sink);
    const SphereGrid g{{slices}, stacks, radius, out.outward() ? 1.0 : -1.0};

    switch (q.draw_style) {
    case GLU_FILL:
        // With texture coordinates every band is a strip (the poles need a
        // different s per triangle); without, the two end bands are fans.
        if (q.texture) {
            for (i32 j = 0; j < stacks; ++j) sphere_band(out, g, j);
        } else {
            sphere_cap(out, g, true);
            sphere_cap(out, g, false);
            for (i32 j = 1; j < stacks - 1; ++j) sphere_band(out, g, j);
        }
        break;
    case GLU_POINT:
        // Every latitude 0 .. stacks, slices points each (the poles repeat).
        out.begin(kPoints);
        for (i32 j = 0; j <= stacks; ++j) {
            for (i32 i = 0; i < slices; ++i) {
                if (out.normals()) out.normal(g.vertex_normal(i, j));
                out.vertex(g.point(i, j), g.around.s_of(i), g.t_of(j));
            }
        }
        out.end();
        break;
    case GLU_LINE:
    case GLU_SILHOUETTE:
        // Latitudes 1 .. stacks - 1 as closed strips, then one meridian per
        // slice from pole to pole. GLU_FLAT normals sit half a slice back on
        // the latitudes and half a stack back on the meridians.
        for (i32 j = 1; j < stacks; ++j) {
            out.begin(kLineStrip);
            for (i32 i = 0; i <= slices; ++i) {
                if (out.smooth()) out.normal(g.vertex_normal(i, j));
                if (out.flat()) out.normal(g.facet_normal(i, true, j));
                out.vertex(g.point(i, j), g.around.s_of(i), g.t_of(j));
            }
            out.end();
        }
        for (i32 i = 0; i < slices; ++i) {
            out.begin(kLineStrip);
            for (i32 j = 0; j <= stacks; ++j) {
                if (out.smooth()) out.normal(g.vertex_normal(i, j));
                if (out.flat()) out.normal(g.facet_normal(i, false, j - 0.5));
                out.vertex(g.point(i, j), g.around.s_of(i), g.t_of(j));
            }
            out.end();
        }
        break;
    default:
        break;
    }
    return 0;
}

// ---- Cylinder --------------------------------------------------------------------------

namespace {

/// Point (i, j) of a cylinder: slice boundary i, stack boundary j = 0 (z = 0,
/// base radius) .. stacks (z = height, top radius).
struct CylinderGrid {
    Around around;
    i32 stacks;
    double base, top, height;
    double normal_xy;   ///< signed: the side normal's length in the xy plane
    double normal_z;    ///< (base - top) / slant, not flipped by orientation

    [[nodiscard]] double radius(i32 j) const noexcept { return base - (base - top) * j / stacks; }
    [[nodiscard]] double z(i32 j) const noexcept { return height * j / stacks; }
    [[nodiscard]] double t_of(i32 j) const noexcept { return static_cast<double>(j) / stacks; }
    [[nodiscard]] Vec3 point(i32 i, i32 j) const noexcept {
        return {radius(j) * around.sin_of(i), radius(j) * around.cos_of(i), z(j)};
    }
    /// The side normal at boundary i (GLU_SMOOTH), or half a slice back
    /// (GLU_FLAT's face normal); @p with_z false drops the z component.
    [[nodiscard]] Vec3 side_normal(i32 i, bool face, bool with_z = true) const noexcept {
        return {normal_xy * around.sin_of(i, face), normal_xy * around.cos_of(i, face), with_z ? normal_z : 0.0};
    }
};

/// One ring of the cylinder at stack boundary @p j as a closed line strip.
void cylinder_ring(Out& out, const CylinderGrid& g, i32 j) {
    out.begin(kLineStrip);
    for (i32 i = 0; i <= g.around.n; ++i) {
        if (out.normals()) out.normal(g.side_normal(i, out.flat()));
        out.vertex(g.point(i, j), g.around.s_of(i), g.t_of(j));
    }
    out.end();
}

}  // namespace

u32 draw_cylinder(const QuadricState& q, double base_radius, double top_radius, double height, i32 slices,
                  i32 stacks, QuadricSink& sink) {
    slices = capped(slices);
    if (slices < 2 || stacks < 1 || base_radius < 0.0 || top_radius < 0.0 || height < 0.0) return GLU_INVALID_VALUE;
    const double slant = std::hypot(base_radius - top_radius, height);
    if (slant == 0.0) return GLU_INVALID_VALUE;   // no side to draw
    Out out(q, sink);
    const double sign = out.outward() ? 1.0 : -1.0;
    const CylinderGrid g{{slices}, stacks, base_radius, top_radius, height, sign * height / slant,
                         (base_radius - top_radius) / slant};

    switch (q.draw_style) {
    case GLU_FILL:
        // A quad strip per stack, a normal per slice boundary (the face's
        // for GLU_FLAT); outward the lower point of each pair comes first.
        for (i32 j = 0; j < stacks; ++j) {
            out.begin(kQuadStrip);
            for (i32 i = 0; i <= slices; ++i) {
                if (out.normals()) out.normal(g.side_normal(i, out.flat()));
                const i32 first = out.outward() ? j : j + 1;
                const i32 second = out.outward() ? j + 1 : j;
                out.vertex(g.point(i, first), g.around.s_of(i), g.t_of(first));
                out.vertex(g.point(i, second), g.around.s_of(i), g.t_of(second));
            }
            out.end();
        }
        break;
    case GLU_POINT:
        // Slice by slice, bottom to top; one (vertex) normal per slice.
        out.begin(kPoints);
        for (i32 i = 0; i < slices; ++i) {
            if (out.normals()) out.normal(g.side_normal(i, false));
            for (i32 j = 0; j <= stacks; ++j) out.vertex(g.point(i, j), g.around.s_of(i), g.t_of(j));
        }
        out.end();
        break;
    case GLU_LINE:
    case GLU_SILHOUETTE:
        // GLU_LINE: the inner rings 1 .. stacks - 1 first. Both: the bottom
        // and top rings, then a line up the side per slice whose normal has
        // no z component (glu-fb-line-cylinder-* p12 .. p19).
        if (q.draw_style == GLU_LINE) {
            for (i32 j = 1; j < stacks; ++j) cylinder_ring(out, g, j);
        }
        cylinder_ring(out, g, 0);
        cylinder_ring(out, g, stacks);
        for (i32 i = 0; i < slices; ++i) {
            if (out.normals()) out.normal(g.side_normal(i, false, false));
            out.begin(kLineStrip);
            for (i32 j = 0; j <= stacks; ++j) out.vertex(g.point(i, j), g.around.s_of(i), g.t_of(j));
            out.end();
        }
        break;
    default:
        break;
    }
    return 0;
}

// ---- Disk and partial disk ---------------------------------------------------------------

namespace {

/// Point (i, j) of a (partial) disk: spoke i = 0 .. slices across the sweep,
/// loop j = 0 (outer radius) .. loops (inner radius).
struct DiskGrid {
    i32 slices, loops;
    double inner, outer;
    double start_radians, sweep_radians;
    bool full_turn;

    [[nodiscard]] double angle(i32 i) const noexcept {
        if (full_turn && i == slices) i = 0;   // the last spoke is the first
        return start_radians + sweep_radians * i / slices;
    }
    [[nodiscard]] double radius(i32 j) const noexcept { return outer - (outer - inner) * j / loops; }
    [[nodiscard]] Vec3 at(double r, i32 i) const noexcept { return {r * std::sin(angle(i)), r * std::cos(angle(i)), 0.0}; }
    /// GLU 1.2: the texture maps the disk of the outer radius onto [0, 1]^2,
    /// (0.5, 0.5) at the centre.
    [[nodiscard]] std::array<double, 2> tex(double r, i32 i) const noexcept {
        const double scale = r / outer / 2;
        return {scale * std::sin(angle(i)) + 0.5, scale * std::cos(angle(i)) + 0.5};
    }
};

void disk_vertex(Out& out, const DiskGrid& g, double r, i32 i) {
    const auto t = g.tex(r, i);
    out.vertex(g.at(r, i), t[0], t[1]);
}

void disk_ring(Out& out, const DiskGrid& g, i32 j) {
    out.begin(kLineStrip);
    for (i32 i = 0; i <= g.slices; ++i) disk_vertex(out, g, g.radius(j), i);
    out.end();
}

void disk_spoke(Out& out, const DiskGrid& g, i32 i) {
    out.begin(kLineStrip);
    for (i32 j = 0; j <= g.loops; ++j) disk_vertex(out, g, g.radius(j), i);
    out.end();
}

}  // namespace

u32 draw_partial_disk(const QuadricState& q, double inner_radius, double outer_radius, i32 slices, i32 loops,
                      double start_angle, double sweep_angle, QuadricSink& sink) {
    slices = capped(slices);
    if (slices < 2 || loops < 1 || outer_radius <= 0.0 || inner_radius < 0.0 || inner_radius > outer_radius) {
        return GLU_INVALID_VALUE;
    }
    // More than a turn either way is one turn (glu-quadric-err-pdisk-sweep-720,
    // -m720: no error); a negative sweep is the same sweep started at its end.
    if (sweep_angle > 360.0 || sweep_angle < -360.0) sweep_angle = 360.0;
    if (sweep_angle < 0.0) {
        start_angle += sweep_angle;
        sweep_angle = -sweep_angle;
    }
    Out out(q, sink);
    const DiskGrid g{slices, loops, inner_radius, outer_radius, start_angle / kHalfTurnDegrees * 3.14159265358979323846,
                     sweep_angle / kHalfTurnDegrees * 3.14159265358979323846, sweep_angle == 360.0};
    // A partial disk has one spoke more than slices; a full one repeats its first.
    const i32 spokes = g.full_turn ? slices : slices + 1;

    // One normal, +z (or -z inward), for the whole disk.
    if (out.normals()) out.normal({0.0, 0.0, out.outward() ? 1.0 : -1.0});

    switch (q.draw_style) {
    case GLU_FILL: {
        // An inner radius of 0 makes the innermost loop a fan round the
        // centre (rim descending outward); the other loops are quad strips,
        // outer radius first outward.
        i32 strips = loops;
        if (inner_radius == 0.0) {
            strips = loops - 1;
            out.begin(kTriangleFan);
            out.vertex({0.0, 0.0, 0.0}, 0.5, 0.5);
            const double r = g.radius(loops - 1);
            for (i32 k = 0; k <= slices; ++k) disk_vertex(out, g, r, out.outward() ? slices - k : k);
            out.end();
        }
        for (i32 j = 0; j < strips; ++j) {
            const double r_first = out.outward() ? g.radius(j) : g.radius(j + 1);
            const double r_second = out.outward() ? g.radius(j + 1) : g.radius(j);
            out.begin(kQuadStrip);
            for (i32 i = 0; i <= slices; ++i) {
                disk_vertex(out, g, r_first, i);
                disk_vertex(out, g, r_second, i);
            }
            out.end();
        }
        break;
    }
    case GLU_POINT:
        out.begin(kPoints);
        for (i32 i = 0; i < spokes; ++i) {
            for (i32 j = 0; j <= loops; ++j) disk_vertex(out, g, g.radius(j), i);
        }
        out.end();
        break;
    case GLU_LINE:
        if (inner_radius == outer_radius) {
            // A ring of zero width: one circle, textured as the full disk.
            out.begin(kLineStrip);
            for (i32 i = 0; i <= slices; ++i) {
                out.vertex(g.at(inner_radius, i), std::sin(g.angle(i)) / 2 + 0.5, std::cos(g.angle(i)) / 2 + 0.5);
            }
            out.end();
            break;
        }
        for (i32 j = 0; j <= loops; ++j) disk_ring(out, g, j);
        for (i32 i = 0; i < spokes; ++i) disk_spoke(out, g, i);
        break;
    case GLU_SILHOUETTE:
        // The two straight edges of a partial disk, then the outer and inner circles.
        if (!g.full_turn) {
            disk_spoke(out, g, 0);
            disk_spoke(out, g, slices);
        }
        disk_ring(out, g, 0);
        if (inner_radius != outer_radius) disk_ring(out, g, loops);
        break;
    default:
        break;
    }
    return 0;
}

}  // namespace vitreo::winapi::glu
