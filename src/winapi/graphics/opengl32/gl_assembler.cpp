// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_assembler.cpp
/// @brief glBegin modes to point/line/triangle lists (gl_assembler.hpp).

#include "winapi/graphics/opengl32/gl_assembler.hpp"

#include "winapi/graphics/opengl32/gl_enums.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace vitreo::winapi::gl {

GlPrimitiveClass gl_primitive_class(u32 mode) noexcept {
    switch (mode) {
    case GL_POINTS: return GlPrimitiveClass::Points;
    case GL_LINES: case GL_LINE_LOOP: case GL_LINE_STRIP: return GlPrimitiveClass::Lines;
    case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
    case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON: return GlPrimitiveClass::Triangles;
    default: return GlPrimitiveClass::None;
    }
}

void GlAssembler::triangle(u32 a, u32 b, u32 c, u8 edges) {
    m_indices.push_back(a);
    m_indices.push_back(b);
    m_indices.push_back(c);
    m_edges.push_back(edges);
}

void GlAssembler::polygon(std::initializer_list<u32> vertices, std::initializer_list<u8> boundary, u32 triangles) {
    m_polygons.push_back({static_cast<u32>(m_polygon_vertices.size()), static_cast<u32>(vertices.size()),
                          static_cast<u32>(m_edges.size()), triangles});
    m_polygon_vertices.insert(m_polygon_vertices.end(), vertices.begin(), vertices.end());
    m_polygon_boundary.insert(m_polygon_boundary.end(), boundary.begin(), boundary.end());
}

void GlAssembler::segment(u32 provoking, u32 other) {
    m_indices.push_back(provoking);
    m_indices.push_back(other);
}

void GlAssembler::emit(u32 mode, std::span<const GlBatchVertex> vertices, std::span<const u8> edge_flags) {
    m_indices.clear();
    m_edges.clear();
    m_polygons.clear();
    m_polygon_vertices.clear();
    m_polygon_boundary.clear();
    m_class = gl_primitive_class(mode);
    const u32 n = static_cast<u32>(vertices.size());
    // Spec 2.6.2: the flag of vertex i says whether the edge it begins is a
    // boundary edge (POLYGON, TRIANGLES, QUADS only). Initially TRUE.
    const auto b = [&](u32 i, u8 bit) -> u8 {
        return (edge_flags.empty() || (i < edge_flags.size() && edge_flags[i] != 0)) ? bit : u8{0};
    };
    constexpr u8 kAll = kGlEdge01 | kGlEdge12 | kGlEdge20;

    switch (mode) {
    case GL_POINTS:
        m_indices.reserve(n);
        for (u32 i = 0; i < n; ++i) m_indices.push_back(i);
        break;
    case GL_LINES:
        // Segment i: vertices 2i-1, 2i (1-based); provoking 2i.
        for (u32 i = 0; i + 1 < n; i += 2) segment(i + 1, i);
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        // Segment i: i, i+1; provoking i+1. A loop adds n -> 1, whose
        // provoking vertex is vertex 1.
        if (n < 2) break;
        for (u32 i = 0; i + 1 < n; ++i) segment(i + 1, i);
        if (mode == GL_LINE_LOOP) segment(0, n - 1);
        break;
    case GL_TRIANGLES:
        // Triangle i: 3i-2, 3i-1, 3i; provoking 3i. Rotated (c, a, b): the
        // edges are c->a (flag of c), a->b (flag of a), b->c (flag of b).
        for (u32 i = 0; i + 2 < n; i += 3) {
            polygon({i, i + 1, i + 2}, {b(i, 1), b(i + 1, 1), b(i + 2, 1)}, 1);
            triangle(i + 2, i, i + 1, static_cast<u8>(b(i + 2, kGlEdge01) | b(i, kGlEdge12) | b(i + 1, kGlEdge20)));
        }
        break;
    case GL_TRIANGLE_STRIP:
        // Triangle i (0-based): (i, i+1, i+2) for even i, (i+1, i, i+2) for odd
        // i; provoking i+2.
        for (u32 i = 0; i + 2 < n; ++i) {
            if ((i & 1u) == 0) {
                polygon({i, i + 1, i + 2}, {1, 1, 1}, 1);
                triangle(i + 2, i, i + 1, kAll);
            } else {
                polygon({i + 1, i, i + 2}, {1, 1, 1}, 1);
                triangle(i + 2, i + 1, i, kAll);
            }
        }
        break;
    case GL_TRIANGLE_FAN:
        // Triangle i: (1, i+1, i+2); provoking i+2.
        for (u32 i = 1; i + 1 < n; ++i) {
            polygon({0, i, i + 1}, {1, 1, 1}, 1);
            triangle(i + 1, 0, i, kAll);
        }
        break;
    case GL_QUADS:
        // Quad i: a b c d = 4i-3 .. 4i, provoking d. (d, a, b) and (d, b, c);
        // the diagonal d-b is not an edge of the quad.
        for (u32 i = 0; i + 3 < n; i += 4) {
            const u32 a = i, bb = i + 1, c = i + 2, d = i + 3;
            polygon({a, bb, c, d}, {b(a, 1), b(bb, 1), b(c, 1), b(d, 1)}, 2);
            triangle(d, a, bb, static_cast<u8>(b(d, kGlEdge01) | b(a, kGlEdge12)));
            triangle(d, bb, c, static_cast<u8>(b(bb, kGlEdge12) | b(c, kGlEdge20)));
        }
        break;
    case GL_QUAD_STRIP:
        // Quad i: boundary order 2i-1, 2i, 2i+2, 2i+1 (1-based) = a b d c,
        // provoking d = 2i+2. Rotated to start at d: d c a b, split along d-a.
        // Every edge of a quad-strip quad is boundary (spec 2.6.2).
        for (u32 i = 0; i + 3 < n; i += 2) {
            const u32 a = i, bb = i + 1, c = i + 2, d = i + 3;
            polygon({a, bb, d, c}, {1, 1, 1, 1}, 2);
            triangle(d, c, a, static_cast<u8>(kGlEdge01 | kGlEdge12));
            triangle(d, a, bb, static_cast<u8>(kGlEdge12 | kGlEdge20));
        }
        break;
    case GL_POLYGON:
        // A fan from vertex 1, the provoking vertex. Edge 1->2 is in the first
        // triangle, k->k+1 in triangle k-1, n->1 in the last.
        if (n < 3) break;
        m_polygons.push_back({0, n, 0, n - 2});
        for (u32 i = 0; i < n; ++i) {
            m_polygon_vertices.push_back(i);
            m_polygon_boundary.push_back(b(i, 1));
        }
        for (u32 i = 1; i + 1 < n; ++i) {
            u8 e = b(i, kGlEdge12);
            if (i == 1) e = static_cast<u8>(e | b(0, kGlEdge01));
            if (i + 2 == n) e = static_cast<u8>(e | b(n - 1, kGlEdge20));
            triangle(0, i, i + 1, e);
        }
        break;
    default:
        m_class = GlPrimitiveClass::None;
        break;
    }
}

// ---- Window coordinates and the CPU point/line expansion ---------------------------

namespace {

std::array<double, 4> apply(const std::array<double, 16>& m, const float p[4]) noexcept {
    std::array<double, 4> r{};
    for (usize row = 0; row < 4; ++row) {
        r[row] = m[0 * 4 + row] * p[0] + m[1 * 4 + row] * p[1] + m[2 * 4 + row] * p[2] + m[3 * 4 + row] * p[3];
    }
    return r;
}

double dot4(const std::array<double, 4>& a, const std::array<double, 4>& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

float lerp(float a, float b, double s) noexcept { return static_cast<float>(a + (b - a) * s); }

/// Whether (u, v) lies in the diamond R_f = {|x - x_f| + |y - y_f| < 1/2}
/// (spec 3.4.1) of the fragment whose square holds it (no other diamond can).
bool in_diamond(double u, double v) noexcept {
    return std::abs(u - (std::floor(u) + 0.5)) + std::abs(v - (std::floor(v) + 0.5)) < 0.5;
}

}  // namespace

u32 gl_aliased_width(float width) noexcept {
    // Spec 3.3 / 3.4.2: rounded to the nearest integer, 0 as if 1, clamped to
    // the implementation maximum (which must be at least the antialiased
    // maximum, 10).
    constexpr float kMaxAliasedWidth = 8192.0f;
    const float w = std::round(std::min(std::max(width, 0.0f), kMaxAliasedWidth));
    return w < 1.0f ? 1u : static_cast<u32>(w);
}

std::array<double, 4> GlWindowMap::to_clip(const float pos[4]) const noexcept { return apply(clip, pos); }
std::array<double, 4> GlWindowMap::to_eye(const float pos[4]) const noexcept { return apply(eye, pos); }

std::array<double, 3> GlWindowMap::to_window(const std::array<double, 4>& c) const noexcept {
    // Spec 2.10.1: x_w = (x_d + 1) p_x / 2 + o_x, z_w = ((f - n) z_d + n + f) / 2.
    const double xd = c[0] / c[3], yd = c[1] / c[3], zd = c[2] / c[3];
    return {(xd + 1.0) * width / 2.0 + x, (yd + 1.0) * height / 2.0 + y,
            ((depth_far - depth_near) * zd + depth_near + depth_far) / 2.0};
}

void GlPrimitiveExpander::quad(const std::array<GlBatchVertex, 4>& corners,
                               const std::array<std::array<double, 2>, 4>& window, bool front) {
    const u32 base = static_cast<u32>(vertices.size());
    for (const GlBatchVertex& c : corners) vertices.push_back(c);
    // Spec 2.13.1 eq. 2.6: a positive area is counter-clockwise in window
    // coordinates. A front quad is wound the way glFrontFace names.
    const double area = (window[1][0] - window[0][0]) * (window[2][1] - window[0][1]) -
                        (window[2][0] - window[0][0]) * (window[1][1] - window[0][1]);
    const bool keep = (area > 0.0) == (front == front_is_ccw);
    static constexpr std::array<u32, 6> kKeep{0, 1, 2, 0, 2, 3}, kFlip{0, 2, 1, 0, 3, 2};
    for (u32 k : keep ? kKeep : kFlip) indices.push_back(base + k);
}

void GlPrimitiveExpander::point(const GlBatchVertex& v, const GlBatchVertex& data, bool front,
                                const float depth_offset[2]) {
    // Spec 2.11: a point outside the clip volume or a user clip plane's
    // half-space is discarded; otherwise it passes unchanged.
    const std::array<double, 4> c = map.to_clip(v.pos);
    if (!(c[3] > 0.0)) return;
    for (u32 k = 0; k < 3; ++k) {
        if (c[k] < -c[3] || c[k] > c[3]) return;
    }
    if (!map.clip_planes.empty()) {
        const std::array<double, 4> e = map.to_eye(v.pos);
        for (const auto& plane : map.clip_planes) {
            if (dot4(plane, e) < 0.0) return;
        }
    }
    const std::array<double, 3> win = map.to_window(c);
    // Spec 3.3: the centre of an odd-width square is (floor(x_w) + 1/2,
    // floor(y_w) + 1/2), of an even one (floor(x_w + 1/2), floor(y_w + 1/2));
    // its sides then lie on pixel edges.
    const u32 w = gl_aliased_width(point_size);
    const bool odd = (w & 1u) != 0;
    const double cx = odd ? std::floor(win[0]) + 0.5 : std::floor(win[0] + 0.5);
    const double cy = odd ? std::floor(win[1]) + 0.5 : std::floor(win[1] + 0.5);
    const double h = w / 2.0;
    const std::array<std::array<double, 2>, 4> window{{{cx - h, cy - h}, {cx + h, cy - h}, {cx + h, cy + h},
                                                       {cx - h, cy + h}}};
    std::array<GlBatchVertex, 4> corners{};
    for (u32 k = 0; k < 4; ++k) {
        GlBatchVertex& q = corners[k];
        q = data;
        std::copy(std::begin(v.pos), std::end(v.pos), q.pos);
        // Flat shading takes the provoking vertex's colour only (2.13.7);
        // texture coordinates stay the point's own (3.3).
        std::copy(std::begin(v.tex), std::end(v.tex), q.tex);
        q.line_distance = 0.0f;
        q.window_offset[0] = static_cast<float>(window[k][0] - win[0]);
        q.window_offset[1] = static_cast<float>(window[k][1] - win[1]);
        q.depth_offset[0] = depth_offset[0];
        q.depth_offset[1] = depth_offset[1];
    }
    quad(corners, window, front);
}

void GlPrimitiveExpander::segment(const GlBatchVertex& a, const GlBatchVertex& b, const GlBatchVertex* flat_data,
                                  bool front, float& counter, const float depth_offset[2]) {
    // Spec 2.11: clip in clip space against the six planes of the clip
    // volume and the user planes; s runs from a (0) to b (1).
    const std::array<double, 4> ca = map.to_clip(a.pos), cb = map.to_clip(b.pos);
    double s0 = 0.0, s1 = 1.0;
    const auto clip_by = [&](double da, double db) {
        if (da < 0.0 && db < 0.0) return false;
        if (da < 0.0) s0 = std::max(s0, da / (da - db));
        if (db < 0.0) s1 = std::min(s1, da / (da - db));
        return s0 <= s1;
    };
    for (u32 k = 0; k < 3; ++k) {
        if (!clip_by(ca[3] + ca[k], cb[3] + cb[k]) || !clip_by(ca[3] - ca[k], cb[3] - cb[k])) return;
    }
    if (!map.clip_planes.empty()) {
        const std::array<double, 4> ea = map.to_eye(a.pos), eb = map.to_eye(b.pos);
        for (const auto& plane : map.clip_planes) {
            if (!clip_by(dot4(plane, ea), dot4(plane, eb))) return;
        }
    }
    const auto at = [&](double s) {
        std::array<double, 4> c{};
        for (u32 k = 0; k < 4; ++k) c[k] = ca[k] + (cb[k] - ca[k]) * s;
        return c;
    };
    const std::array<double, 4> c0 = at(s0), c1 = at(s1);
    if (!(c0[3] > 0.0) || !(c1[3] > 0.0)) return;
    const std::array<double, 3> p0 = map.to_window(c0), p1 = map.to_window(c1);

    // Spec 3.4.1: x-major when |dx| >= |dy|. (u, v) = (major, minor).
    const bool x_major = std::abs(p1[0] - p0[0]) >= std::abs(p1[1] - p0[1]);
    const double u0 = x_major ? p0[0] : p0[1], v0 = x_major ? p0[1] : p0[0];
    const double u1 = x_major ? p1[0] : p1[1], v1 = x_major ? p1[1] : p1[0];
    if (u0 == u1) return;   // then v0 == v1 too: nothing exits a diamond
    // Spec 3.4.2: a line of width w rasterises the segment offset by
    // -(w - 1)/2 in the minor direction and replicates each fragment w times
    // in +minor. The diamond-exit rule on the offset segment (|slope| <= 1):
    // each column whose centre it crosses produces the fragment whose
    // diamond holds the crossing; the start's own diamond adds its column
    // when that centre lies behind the start; the end's diamond is never
    // produced (the segment is half-open).
    const double w = gl_aliased_width(line_width);
    const double shift = (w - 1.0) / 2.0;
    const bool d0 = in_diamond(u0, v0 - shift), d1 = in_diamond(u1, v1 - shift);
    const double f0 = std::floor(u0), f1 = std::floor(u1);
    double first, last, n, ua, ub;   // columns; ua / ub: their outer edges, in segment order
    if (u1 > u0) {
        first = std::ceil(u0 - 0.5);
        last = std::floor(u1 - 0.5);
        if (d0 && f0 + 0.5 < u0) first = f0;
        if (d1 && f1 + 0.5 <= u1) last = f1 - 1.0;
        n = last - first + 1.0;
        ua = first;
        ub = last + 1.0;
    } else {
        first = std::floor(u0 - 0.5);
        last = std::ceil(u1 - 0.5);
        if (d0 && f0 + 0.5 > u0) first = f0;
        if (d1 && f1 + 0.5 >= u1) last = f1 + 1.0;
        n = first - last + 1.0;
        ua = first + 1.0;
        ub = last;
    }
    if (n <= 0.0) return;

    // So at each produced column the w fragments whose centres lie within
    // w/2 of the segment in the minor direction: the parallelogram between
    // the column edges ua, ub and the segment's line offset by +-w/2.
    const double h = w / 2.0;
    const double du = u1 - u0, dv = v1 - v0;
    const double ends[2] = {ua, ub};
    const float end_counter[2] = {counter, counter + static_cast<float>(n)};
    std::array<GlBatchVertex, 4> corners{};
    std::array<std::array<double, 2>, 4> window{};
    for (u32 k = 0; k < 4; ++k) {
        // Cycle order: start edge (-, +), end edge (+, -).
        const u32 e = k / 2;
        const double sign = (k == 0 || k == 3) ? -1.0 : 1.0;
        const double cu = ends[e];
        const double cv = v0 + (cu - u0) / du * dv + sign * h;
        // Data of a wide line's fragments. 3.4.2 says only that "a column of
        // fragments of height w" is produced at each x, i.e. every fragment
        // of the column is a produced fragment, and 3.4.1 defines the data of
        // a produced fragment from its own centre: "Let the window
        // coordinates of a produced fragment center be given by p_r", t =
        // (p_r - p_a) . (p_b - p_a) / |p_b - p_a|^2 (eq. 3.1). So each
        // fragment of a column projects its own centre (not its column's base
        // fragment's); depth likewise by eq. 3.3. (Deliberate reading;
        // GlRasterTest.WideAliasedLineCoversSpecPixels pins it.)
        // Eq. 3.1: a fragment's datum is the segment's at t, the projection
        // of its centre onto the segment, an affine function of the window
        // position; so is 1/w along the segment (eq. 3.2). Each corner takes
        // the object position and data of its own projection's point, and
        // the backend's perspective-correct interpolation across the quad
        // reproduces eq. 3.2 (and eq. 3.3 for depth) at every fragment.
        double t = ((cu - u0) * du + (cv - v0) * dv) / (du * du + dv * dv);
        double denominator = (1.0 - t) * c1[3] + t * c0[3];
        if (!(denominator > 0.0)) {
            t = std::clamp(t, 0.0, 1.0);
            denominator = (1.0 - t) * c1[3] + t * c0[3];
        }
        const double s = s0 + t * c0[3] / denominator * (s1 - s0);
        GlBatchVertex& q = corners[k];
        for (u32 j = 0; j < 4; ++j) q.pos[j] = lerp(a.pos[j], b.pos[j], s);
        for (u32 j = 0; j < 4; ++j) q.color[j] = flat_data ? flat_data->color[j] : lerp(a.color[j], b.color[j], s);
        for (u32 j = 0; j < 3; ++j) q.normal[j] = flat_data ? flat_data->normal[j] : lerp(a.normal[j], b.normal[j], s);
        // Flat shading is the colour's only (2.13.7): texture coordinates are
        // interpolated (eq. 3.1) on flat-shaded segments too. Every corner
        // takes its projection's coordinates, so the fragment stage's lambda
        // (eq. 3.11, the polygon formula, ff_emulation.cpp) sees derivatives
        // along the segment only, not eq. 3.12's directional derivative.
        for (u32 j = 0; j < 4; ++j) q.tex[j] = lerp(a.tex[j], b.tex[j], s);
        // The stipple counter runs along the major axis, one per column.
        q.line_distance = end_counter[e];
        // The corner minus its projection's point on the segment.
        const double ou = cu - (u0 + t * du), ov = cv - (v0 + t * dv);
        q.window_offset[0] = static_cast<float>(x_major ? ou : ov);
        q.window_offset[1] = static_cast<float>(x_major ? ov : ou);
        q.depth_offset[0] = depth_offset[0];
        q.depth_offset[1] = depth_offset[1];
        window[k] = x_major ? std::array<double, 2>{cu, cv} : std::array<double, 2>{cv, cu};
    }
    counter = end_counter[1];
    quad(corners, window, front);
}

}  // namespace vitreo::winapi::gl
