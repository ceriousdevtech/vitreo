// SPDX-License-Identifier: LGPL-2.1-only
/// @file raster_emulation.hpp
/// @brief Pass C of target-independent and conservative rasterization: the
///        vertex shader that draws each captured triangle as the region its
///        candidate pixels' centres lie in (raster_emulation_types.hpp).
///
/// The shader reads its triangle's three records the way the other pass-C
/// shaders do (gs_passthrough.hpp): a vertex capture through 32-bit record
/// ids, a geometry expansion through per-invocation regions, or a
/// tessellated domain through the tessellator's index list. For each
/// triangle it draws 9 vertices -- up to three Metal triangles, a fan over
/// a convex polygon -- and every vertex carries the same flat data, the
/// triangle's snapped edge functions, for the pixel shader's coverage test.
///
/// Geometry (window pixels, y down):
///   - every vertex is snapped to 16.8 fixed point, round to nearest even
///     (D3D11.3 3.4.1; the TIR rows of d3d12_a31_raster_probe follow it on
///     both oracles), and the edge functions are exact 64-bit integers over
///     1/512-pixel coordinates;
///   - the region is the triangle with each edge pushed out by h times its
///     L1 normal -- h = 1/2 pixel plus the 1/512 snapping slack for a
///     one-sample pass, 1 pixel for a multisampled one so that every Metal
///     sample of a candidate pixel is rasterized -- or, when that is
///     ill-conditioned (a sliver), the snapped bounding box grown by h;
///   - every region vertex is placed in the triangle's plane: its clip-space
///     w is 1/q, q = 1/w being affine in window space, so perspective
///     interpolation of every attribute stays on the triangle's plane
///     (conservative rasterization extrapolates, as the spec allows);
///     noperspective attributes are placed by window-space barycentrics,
///     constant ones take vertex 0's value;
///   - a triangle with a vertex at w <= 0 is drawn as the viewport, clipped
///     to where q > 0, and tested against 1/w > 0 per sample;
///   - a post-snap degenerate triangle (tier 2) is drawn as its grown
///     bounding box, back-facing, with vertex 0's values everywhere
///     ("degenerate triangles must use the values assigned to Vertex 0");
///     target-independent rasterization culls it, as ordinary rasterization.
/// The pipeline draws without depth clipping: the pixel shader clips
/// 0 <= z/w <= 1 itself (kRasterTriangleDepthClip), per sample, so a region
/// grown past the near or far plane is not cut where its pixels are not.

#pragma once

#include "gs_passthrough.hpp"
#include "raster_emulation_types.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// t2 of a raster-emulation pass C: the per-draw state its shader reads.
struct RasterEmulationParams {
    /// D3D primitives the draw holds; pass C draws 9 vertices for each.
    u32 primitives{0};
    /// kRasterParam* bits.
    u32 flags{0};
    /// RasterEmulationSource::gpu_primitive_limit: the word of t3 holding
    /// how many primitives the GPU produced.
    u32 limit_word{0};
    u32 reserved{0};
    /// Each viewport's (Width, Height, TopLeftX, TopLeftY).
    std::array<std::array<float, 4>, 16> viewports{};
};
static_assert(sizeof(RasterEmulationParams) == 16 + 16 * 16);

/// Conservative rasterization (else target-independent).
inline constexpr u32 kRasterParamConservative = 1u << 0;
/// DepthClipEnable.
inline constexpr u32 kRasterParamDepthClip = 1u << 1;
/// The pass is multisampled: grow regions by a whole pixel.
inline constexpr u32 kRasterParamMultisampled = 1u << 2;
/// FrontCounterClockwise (a degenerate triangle is drawn back-facing).
inline constexpr u32 kRasterParamFrontCounterClockwise = 1u << 3;

/// Metal vertices pass C draws per D3D triangle (three triangles).
inline constexpr u32 kRasterVerticesPerPrimitive = 9;
/// Metal triangles per D3D triangle: SV_PrimitiveID is Metal's primitive id
/// divided by it (DxbcTranslationOptions::line_quad_primitive_divisor).
inline constexpr u32 kRasterTrianglesPerPrimitive = 3;

/// Where the triangles' records come from.
struct RasterEmulationSource {
    /// 0: three 32-bit record ids per triangle at t1 (a vertex capture, or a
    /// tessellator index list). Nonzero: consecutive slots of
    /// per-invocation regions of this many vertices whose written counts
    /// are at t1, laid out as for generate_gs_passthrough_vertex_msl (a
    /// geometry expansion).
    u32 vertices_per_invocation{0};
    /// Bound t3: the draw stops at min(primitives, t3[limit_word]).
    bool gpu_primitive_limit{false};
    /// Triangle p is records 3p .. 3p + 2 (a mesh shader's raster capture,
    /// mesh_stage.hpp); t1 is not read. Only with vertices_per_invocation 0.
    bool consecutive_records{false};
    /// The v# registers the pixel shader reads with GetAttributeAtVertex:
    /// each delivered flat at per_vertex_input_location(r, 0..2) from the
    /// primitive's records in D3D vertex order (as
    /// generate_gs_passthrough_vertex_msl does); past a line's or point's
    /// last vertex, 0.
    u32 per_vertex_registers{0};
    /// DxbcToSpirvResult::raster_barycentrics of the pixel shader.
    u32 barycentrics{0};
    /// What each D3D primitive is drawn as, and its records: 3 (triangle),
    /// 2 (line: Quad or AliasedLine), 1 (point: Quad). The ids at t1 and the
    /// region slots come in groups of primitive_vertices.
    RasterPrimitive primitive{RasterPrimitive::Triangle};
    u32 primitive_vertices{3};
    /// A Quad line's width: 1.4 (QUADRILATERAL_WIDE), 1.0 (NARROW, and the
    /// conservative coverage of an aliased line).
    float line_width{1.4f};
    /// RasterEmulationOptions::edges_in_buffer: the edge data is not a
    /// varying; generate_raster_emulation_edge_kernel_msl writes it.
    bool edges_in_buffer{false};
};

/// Int4 slots of one primitive's edge data in the edge buffer
/// (RasterEmulationOptions::edges_in_buffer): edges 0-2, bounds, 1/w
/// plane, z/w plane, edge 3, one spare.
inline constexpr u32 kRasterEdgeBufferSlots = 8;
/// Entry point of the edge kernel.
inline constexpr const char* kRasterEdgeKernelEntryPoint = "vitreo_raster_edges";

/// How the pixel shader interpolates an input register.
enum class RasterVarying : u8 {
    Perspective = 0,
    NoPerspective = 1,
    Constant = 2,
};

/// Pass C of a line or point (RasterPrimitive Quad / AliasedLine), first
/// half: from the records' clip positions c0, c1 (c1 == c0 for a point) to
/// the edge data e[0..2], e3, bounds, Q (1/w), Z (z/w) and tri_flags.
///   - The primitive is clipped in clip space to w > 0 and to the
///     rasterizer's range (D3D clips every primitive; window coordinates
///     must fit 16.8), keeping each new endpoint's weights over the two
///     records (u0, u1): attributes at a clipped vertex are the clip-space
///     interpolation of the originals.
///   - Endpoints snap to 16.8 (round to nearest even, as triangles).
///   - Attributes of a line "vary along the length, but are constant across
///     the perpendicular" (D3D11.3 15.14.1): over window pixels the line's
///     parameter is the affine t = T . (x, y, 1) (the projection on the
///     segment), and 1/w, z/w are affine in t. A point's are constant.
///   - Quad: a point is the width-1 square centred on it (3.4.6), a line the
///     rectangle of line_width about the segment (3.4.5), corners snapped;
///     four edges as a triangle's, the top-left rule folded in for TIR.
///   - AliasedLine: e[0] = (A, B) snapped, 1/512 pixel; a zero-length one
///     never exits a diamond (3.4.3) and draws nothing.
[[nodiscard]] inline std::string raster_emulation_line_point_msl(
    const RasterEmulationSource& source) {
    const bool point = source.primitive_vertices <= 1;
    std::string out;
    out += R"MSL(    float4 a4 = c0;
    float4 b4 = c1;
    float2 u0 = float2(1.0, 0.0);
    float2 u1 = float2(0.0, 1.0);
    {
        const float eps = max(abs(c0.w), abs(c1.w)) * 1.0e-6;
        const float K = 16000.0 / max(max(size.x, size.y) * 0.5, 1.0);
        const float4 dc = c1 - c0;
        const float pa[5] = {c0.w - eps, K * c0.w - c0.x, K * c0.w + c0.x,
                             K * c0.w - c0.y, K * c0.w + c0.y};
        const float pd[5] = {dc.w, K * dc.w - dc.x, K * dc.w + dc.x,
                             K * dc.w - dc.y, K * dc.w + dc.y};
        float t0 = 0.0;
        float t1 = 1.0;
        for (uint i = 0u; i < 5u; ++i) {
            if (pd[i] == 0.0) {
                if (!(pa[i] >= 0.0)) VITREO_RETURN;
                continue;
            }
            const float t = -pa[i] / pd[i];
            if (pd[i] > 0.0) {
                t0 = max(t0, t);
            } else {
                t1 = min(t1, t);
            }
        }
        if (!(t0 <= t1)) VITREO_RETURN;
        a4 = c0 + dc * t0;
        b4 = c0 + dc * t1;
        u0 = float2(1.0 - t0, t0);
        u1 = float2(1.0 - t1, t1);
    }
    const float2 sa = float2((a4.x / a4.w + 1.0) * (size.x * 0.5) + origin.x,
                             (1.0 - a4.y / a4.w) * (size.y * 0.5) + origin.y);
    const float2 sb = float2((b4.x / b4.w + 1.0) * (size.x * 0.5) + origin.x,
                             (1.0 - b4.y / b4.w) * (size.y * 0.5) + origin.y);
    const int2 SA = int2(rint(sa * 256.0)) * 2;
    const int2 SB = int2(rint(sb * 256.0)) * 2;
    const float2 fa = float2(SA) / 512.0;
    const float2 fb = float2(SB) / 512.0;
    const float qa = 1.0 / a4.w;
    const float qb = 1.0 / b4.w;
    const float za = a4.z / a4.w;
    const float zb = b4.z / b4.w;
    const uint tri_flags = (flags & 2u) != 0u ? 1u : 0u;
    int4 e[3] = {int4(0), int4(0), int4(0)};
    int4 bounds = int4(min(SA, SB), max(SA, SB));
    const float2 dl = fb - fa;
    const float len2 = dot(dl, dl);
    float3 T = float3(0.0);
    if (len2 > 0.0) T = float3(dl / len2, -dot(fa, dl) / len2);
)MSL";
    if (source.primitive == RasterPrimitive::AliasedLine) {
        // An aliased line's attributes follow its major axis (measured:
        // d3d12_a66_c6_probe "lines tir1", WARP12 and Metal's own lines
        // alike, t = (x - A.x) / (B.x - A.x) for an x-major line).
        out += R"MSL(    if (abs(dl.x) >= abs(dl.y)) {
        if (dl.x != 0.0) T = float3(1.0 / dl.x, 0.0, -fa.x / dl.x);
    } else {
        T = float3(0.0, 1.0 / dl.y, -fa.y / dl.y);
    }
)MSL";
    }
    out += R"MSL(    const float3 Q = float3(0.0, 0.0, qa) + (qb - qa) * T;
    const float3 Z = float3(0.0, 0.0, za) + (zb - za) * T;
)MSL";
    if (source.primitive == RasterPrimitive::AliasedLine) {
        out += R"MSL(    if (all(SA == SB)) VITREO_RETURN;
    e[0] = int4(SA, SB);
)MSL";
        return out;
    }
    if (point) {
        out += R"MSL(    const int2 K4[4] = {SA + int2(-256, -256), SA + int2(256, -256),
                        SA + int2(256, 256), SA + int2(-256, 256)};
)MSL";
    } else {
        char width[32];
        std::snprintf(width, sizeof(width), "%.9g", static_cast<double>(source.line_width) * 0.5);
        out += "    if (!(len2 > 0.0)) VITREO_RETURN;\n";
        out += "    const float2 half_n = float2(-dl.y, dl.x) * rsqrt(len2) * " + std::string(width) + ";\n";
        out += R"MSL(    const int2 K4[4] = {int2(rint((fa - half_n) * 256.0)) * 2,
                        int2(rint((fb - half_n) * 256.0)) * 2,
                        int2(rint((fb + half_n) * 256.0)) * 2,
                        int2(rint((fa + half_n) * 256.0)) * 2};
)MSL";
    }
    out += R"MSL(    long area4 = 0;
    for (uint i = 0u; i < 4u; ++i) {
        const uint j = (i + 1u) & 3u;
        area4 += long(K4[i].x) * long(K4[j].y) - long(K4[j].x) * long(K4[i].y);
    }
    if (area4 == 0) VITREO_RETURN;
    const long sign4 = area4 > 0 ? 1 : -1;
    int4 E4[4];
    for (uint i = 0u; i < 4u; ++i) {
        const uint j = (i + 1u) & 3u;
        const long A = -long(K4[j].y - K4[i].y) * sign4;
        const long B = long(K4[j].x - K4[i].x) * sign4;
        long C = -(A * long(K4[i].x) + B * long(K4[i].y));
        const bool top_left = A > 0 || (A == 0 && B > 0);
        if (!conservative && !top_left) C -= 1;
        E4[i] = vitreo_pack_edge(A, B, C);
    }
    e[0] = E4[0];
    e[1] = E4[1];
    e[2] = E4[2];
    e3 = E4[3];
    bounds = int4(min(min(K4[0], K4[1]), min(K4[2], K4[3])),
                  max(max(K4[0], K4[1]), max(K4[2], K4[3])));
)MSL";
    return out;
}

/// Pass C of a line or point, second half: the region (convex, four
/// corners: the quadrilateral grown by h along its L1 normals, or the
/// aliased line's rectangle reaching h * sqrt(2) past it -- every pixel
/// centre whose diamond it can exit), wound as front-facing ("cull modes do
/// not apply" to points and lines; the draw culls nothing), and the corner's
/// position and weights over the records (beta, lambda).
[[nodiscard]] inline std::string raster_emulation_line_point_region_msl(
    const RasterEmulationSource& source) {
    std::string out;
    out += "    float2 poly[5];\n    uint n = 4u;\n";
    if (source.primitive == RasterPrimitive::AliasedLine) {
        out += R"MSL(    {
        const float r = h * 1.41421356;
        const float2 dir = dl * rsqrt(len2) * r;
        const float2 nrm = float2(-dir.y, dir.x);
        poly[0] = fa - dir - nrm;
        poly[1] = fb + dir - nrm;
        poly[2] = fb + dir + nrm;
        poly[3] = fa - dir + nrm;
    }
)MSL";
    } else {
        out += R"MSL(    {
        float2 Kf[4];
        for (uint i = 0u; i < 4u; ++i) Kf[i] = float2(K4[i]) / 512.0;
        const float o = area4 > 0 ? 1.0 : -1.0;
        float3 E[4];
        for (uint i = 0u; i < 4u; ++i) {
            const float3 l = cross(float3(Kf[i], 1.0), float3(Kf[(i + 1u) & 3u], 1.0)) * o;
            E[i] = l + float3(0.0, 0.0, h * (abs(l.x) + abs(l.y)));
        }
        bool sound = true;
        for (uint i = 0u; i < 4u; ++i) {
            const float3 v = cross(E[(i + 3u) & 3u], E[i]);
            if (!(abs(v.z) > 0.0)) {
                sound = false;
                break;
            }
            poly[i] = v.xy / v.z;
            if (!(distance(poly[i], Kf[i]) < 16.0)) sound = false;
        }
        if (!sound) {
            const float2 lo = min(min(Kf[0], Kf[1]), min(Kf[2], Kf[3])) - h;
            const float2 hi = max(max(Kf[0], Kf[1]), max(Kf[2], Kf[3])) + h;
            poly[0] = lo;
            poly[1] = float2(hi.x, lo.y);
            poly[2] = hi;
            poly[3] = float2(lo.x, hi.y);
        }
    }
)MSL";
    }
    out += R"MSL(    {
        // Front-facing: clockwise on the target unless FrontCounterClockwise.
        float wind = 0.0;
        for (uint i = 0u; i < 4u; ++i) {
            const float2 p = poly[i];
            const float2 q = poly[(i + 1u) & 3u];
            wind += p.x * q.y - q.x * p.y;
        }
        const bool want_clockwise = (flags & 8u) == 0u;
        if ((wind > 0.0) != want_clockwise) {
            const float2 t1 = poly[1];
            poly[1] = poly[3];
            poly[3] = t1;
        }
    }
    const uint fan = corner / 3u;
    const uint k = corner - fan * 3u;
    if (fan + 2u >= n) return out;
    // On the 16.8 grid, so the rasterizer's own snapping moves no corner off
    // the values computed for it (h holds 1/256 pixel of slack for it).
    const float2 v = rint(poly[k == 0u ? 0u : fan + k] * 256.0) / 256.0;
    const float2 ndc = float2((v.x - origin.x) / (size.x * 0.5) - 1.0,
                              1.0 - (v.y - origin.y) / (size.y * 0.5));
    const float t = dot(T, float3(v, 1.0));
    const float2 le = float2(1.0 - t, t);
    const float q = max(le.x * qa + le.y * qb, max(qa, qb) * 1.0e-6);
    const float w = 1.0 / q;
    const float2 be = le * float2(qa, qb) * w;
    const float3 beta = float3(be.x * u0 + be.y * u1, 0.0);
    const float3 lambda = float3(le.x * u0 + le.y * u1, 0.0);
    out.position = float4(ndc * w, dot(Z, float3(v, 1.0)) * w, w);
)MSL";
    return out;
}

/// Generate the pass-C vertex shader over the records @p outputs describe
/// (as for generate_gs_passthrough_vertex_msl). @p modes gives each o#'s
/// interpolation in the pixel shader (integer registers are constant
/// whatever it says). Draw kRasterVerticesPerPrimitive vertices per
/// triangle, non-indexed, triangle list, with the t2 parameters. Empty when
/// the records hold no position. With @p edge_kernel, the compute kernel
/// (kRasterEdgeKernelEntryPoint, one thread per primitive, the same buffers
/// plus the edge buffer at BACKEND_SRV_BUFFER_BASE + 4) that writes each
/// primitive's edge data instead (RasterEmulationSource::edges_in_buffer).
[[nodiscard]] inline std::string raster_emulation_msl(
    const std::vector<GsPassthroughOutput>& outputs,
    const RasterEmulationSource& source,
    const std::array<RasterVarying, 32>& modes, bool fixed_function_push,
    bool edge_kernel) {
    const u32 per_vertex_registers = source.per_vertex_registers;
    const bool tri = source.primitive == RasterPrimitive::Triangle;
    const bool quad = source.primitive == RasterPrimitive::Quad;
    const u32 n_records = tri ? 3u : std::clamp(source.primitive_vertices, 1u, 2u);
    const bool edge_varyings = !source.edges_in_buffer && !edge_kernel;
    const auto record = gs_record_order(outputs);
    usize position_index = record.size();
    for (usize i = 0; i < record.size(); ++i) {
        if (record[i].is_position && record[i].delivered) position_index = i;
    }
    if (record.empty() || position_index == record.size()) return {};
    const auto msl_type = [](const GsPassthroughOutput& o) -> const char* {
        if (o.component_type == 1u) return "uint4";
        if (o.component_type == 2u) return "int4";
        return "float4";
    };
    struct Delivered {
        usize record_index;
        u32 component;
    };
    std::vector<Delivered> clips;
    std::optional<Delivered> layer;
    std::optional<Delivered> viewport;
    for (usize i = 0; i < record.size(); ++i) {
        if (!record[i].delivered) continue;
        for (u32 c = 0; c < 4; ++c) {
            const u8 sv = record[i].component_system_values[c];
            if (sv == 2u) clips.push_back({i, c});
            if (sv == 4u) layer = Delivered{i, c};
            if (sv == 5u) viewport = Delivered{i, c};
        }
    }
    const std::string stride = std::to_string(record.size()) + "u";
    const std::string pos = std::to_string(position_index) + "u";
    static constexpr const char* kLane[4] = {"x", "y", "z", "w"};
    const auto loc = [](u32 location) {
        return "[[user(locn" + std::to_string(location) + ")]]";
    };

    std::string out;
    out += "#include <metal_stdlib>\n";
    out += "using namespace metal;\n\n";
    out += "// Generated by vitreo: target-independent / conservative "
           "rasterization, pass C.\n\n";
    out += "struct vitreo_raster_params {\n"
           "    uint primitives;\n"
           "    uint flags;\n"
           "    uint limit_word;\n"
           "    uint reserved;\n"
           "    float4 viewport[16];\n"
           "};\n\n";
    out += "struct vitreo_gs_passthrough_out {\n";
    for (const auto& o : record) {
        if (!o.delivered) continue;
        if (o.is_position) {
            out += "    float4 position [[position]];\n";
        } else {
            out += std::string("    ") + msl_type(o) + " locn" +
                   std::to_string(o.register_num) + " " + loc(o.register_num) +
                   ";\n";
        }
    }
    if (edge_varyings) {
        for (u32 i = 0; i < 3; ++i) {
            out += "    int4 vitreo_edge" + std::to_string(i) + " " +
                   loc(kRasterEdgeLocation + i) + ";\n";
        }
        out += "    int4 vitreo_bounds " + loc(kRasterBoundsLocation) + ";\n";
        out += "    float4 vitreo_inverse_w " + loc(kRasterInversePlaneLocation) +
               ";\n";
        out += "    float4 vitreo_depth " + loc(kRasterDepthPlaneLocation) + ";\n";
        if (quad) out += "    int4 vitreo_edge3 " + loc(kRasterEdge3Location) + ";\n";
    }
    if (layer.has_value()) {
        out += "    uint vitreo_layer [[render_target_array_index]];\n";
    }
    if (viewport.has_value()) {
        out += "    uint vitreo_viewport [[viewport_array_index]];\n";
    }
    if (!clips.empty()) {
        out += "    float vitreo_clip [[clip_distance]] [" +
               std::to_string(clips.size()) + "];\n";
    }
    // GetAttributeAtVertex copies (flat); a register the records lack reads 0.
    const auto per_vertex_record = [&](u32 reg) -> std::optional<usize> {
        for (usize i = 0; i < record.size(); ++i) {
            if (record[i].register_num == reg) return i;
        }
        return std::nullopt;
    };
    const auto per_vertex_type = [&](u32 reg) -> const char* {
        const auto i = per_vertex_record(reg);
        return i.has_value() ? msl_type(record[*i]) : "float4";
    };
    for (u32 reg = 0; reg < 32; ++reg) {
        if ((per_vertex_registers & (1u << reg)) == 0) continue;
        for (u32 k = 0; k < 3; ++k) {
            out += std::string("    ") + per_vertex_type(reg) + " vitreo_pv" +
                   std::to_string(reg) + "_" + std::to_string(k) + " " +
                   loc(per_vertex_input_location(reg, k)) + ";\n";
        }
    }
    // SV_Barycentrics: each region vertex's weights over the primitive.
    if ((source.barycentrics & 1u) != 0) {
        out += "    float4 vitreo_bary " + loc(kRasterBaryLocation) + ";\n";
    }
    if ((source.barycentrics & 2u) != 0) {
        out += "    float4 vitreo_bary_np " + loc(kRasterBaryNoPerspectiveLocation) +
               " [[center_no_perspective]];\n";
    }
    out += "};\n\n";

    // A Metal triangle that draws nothing.
    out += "static inline void vitreo_hide(thread vitreo_gs_passthrough_out& o) "
           "{ o.position = float4(2.0, 2.0, 2.0, 1.0); }\n\n";
    // An edge function's exact value at (X, Y), 1/512 pixel.
    out += "static inline int4 vitreo_pack_edge(long a, long b, long c) {\n"
           "    return int4(int(a), int(b), int(uint(ulong(c) & 0xFFFFFFFFul)), "
           "int(c >> 32));\n"
           "}\n\n";

    if (edge_kernel) {
        out += std::string("kernel void ") + kRasterEdgeKernelEntryPoint + "(\n";
        out += "    uint vitreo_prim [[thread_position_in_grid]],\n";
        out += "    device int4* vitreo_edge_out [[buffer(" +
               std::to_string(BACKEND_SRV_BUFFER_BASE + 4) + ")]],\n";
    } else {
        out += std::string("vertex vitreo_gs_passthrough_out ") +
               kGsPassthroughEntryPoint + "(\n";
        out += "    uint vitreo_vertex_id [[vertex_id]],\n";
    }
    out += "    const device float4* vitreo_gs_out [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n";
    out += "    const device uint* vitreo_gs_counts [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE + 1) + ")]],\n";
    out += "    constant vitreo_raster_params& vitreo_raster [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE + 2) + ")]]";
    if (source.gpu_primitive_limit) {
        out += ",\n    const device uint* vitreo_limit [[buffer(" +
               std::to_string(BACKEND_SRV_BUFFER_BASE + 3) + ")]]";
    }
    if (fixed_function_push && !edge_kernel) {
        out += ",\n    constant float* vitreo_push [[buffer(0)]]";
    }
    out += ")\n{\n";
    if (edge_kernel) {
        out += "#define VITREO_RETURN return\n";
        out += "    const uint prim = vitreo_prim;\n";
    } else {
        out += "#define VITREO_RETURN return out\n";
        out += "    vitreo_gs_passthrough_out out = {};\n";
        out += "    vitreo_hide(out);\n";
        out += "    const uint prim = vitreo_vertex_id / 9u;\n";
        out += "    const uint corner = vitreo_vertex_id - prim * 9u;\n";
    }
    out += "    uint count = vitreo_raster.primitives;\n";
    if (source.gpu_primitive_limit) {
        out += "    count = min(count, vitreo_limit[vitreo_raster.limit_word]);\n";
    }
    out += "    if (prim >= count) VITREO_RETURN;\n";
    // The primitive's records; a line's or point's missing ones repeat its
    // first (their weights are 0).
    const std::string n = std::to_string(n_records) + "u";
    out += "    uint r[3];\n";
    if (source.vertices_per_invocation != 0) {
        const std::string capacity =
            std::to_string(source.vertices_per_invocation) + "u";
        out += "    const uint slot0 = prim * " + n + ";\n";
        out += "    const uint invocation = slot0 / " + capacity + ";\n";
        out += "    if (slot0 - invocation * " + capacity + " + " + n +
               " > vitreo_gs_counts[" + std::to_string(kGsInvocationCountBase) +
               "u + invocation]) VITREO_RETURN;\n";
        out += "    for (uint k = 0u; k < 3u; ++k) r[k] = slot0 + (k < " + n +
               " ? k : 0u);\n";
    } else if (source.consecutive_records) {
        // A mesh shader's raster capture: primitive p is records
        // p * n .. p * n + n - 1.
        out += "    for (uint k = 0u; k < 3u; ++k) r[k] = prim * " + n + " + (k < " + n +
               " ? k : 0u);\n";
    } else {
        out += "    for (uint k = 0u; k < 3u; ++k) r[k] = "
               "vitreo_gs_counts[prim * " + n + " + (k < " + n + " ? k : 0u)];\n";
    }
    out += "#define VITREO_RECORD(v, i) vitreo_gs_out[(v) * " + stride +
           " + (i)]\n";
    out += "    const float4 c0 = VITREO_RECORD(r[0], " + pos + ");\n";
    out += "    const float4 c1 = VITREO_RECORD(r[1], " + pos + ");\n";
    out += "    const float4 c2 = VITREO_RECORD(r[2], " + pos + ");\n";
    if (viewport.has_value()) {
        out += "    const uint vp_raw = as_type<uint4>(VITREO_RECORD(r[0], " +
               std::to_string(viewport->record_index) + "u))." +
               kLane[viewport->component] + ";\n";
        out += "    const uint vp_index = min(vp_raw, 15u);\n";
    } else {
        out += "    const uint vp_index = 0u;\n";
    }
    out += R"MSL(    const float2 size = vitreo_raster.viewport[vp_index].xy;
    const float2 origin = vitreo_raster.viewport[vp_index].zw;
    const uint flags = vitreo_raster.flags;
    const bool conservative = (flags & 1u) != 0u;
    const float h = (flags & 4u) != 0u ? 1.0 + 1.0 / 256.0
                                       : 0.5 + 1.0 / 256.0;
    int4 e3 = int4(0);
)MSL";
    if (!tri) {
        out += raster_emulation_line_point_msl(source);
    } else {
        out += R"MSL(    const float4 c[3] = {c0, c1, c2};
    // D3D clips every primitive to w > 0; a vertex at or behind the eye has
    // no window position to snap.
    const bool clip_w = !(c0.w > 0.0 && c1.w > 0.0 && c2.w > 0.0);
    // Window positions, snapped to 16.8 (round to nearest even), as
    // 1/512-pixel integers. Out of the rasterizer's range, or behind the
    // eye, the edges are taken from the homogeneous lines instead.
    float2 s[3];
    int2 S[3];
    bool exact = !clip_w;
    for (uint k = 0u; k < 3u; ++k) {
        s[k] = float2((c[k].x / c[k].w + 1.0) * (size.x * 0.5) + origin.x,
                      (1.0 - c[k].y / c[k].w) * (size.y * 0.5) + origin.y);
        if (!(all(abs(s[k]) < float2(32768.0)))) exact = false;
    }
    if (exact) {
        for (uint k = 0u; k < 3u; ++k) {
            S[k] = int2(rint(s[k] * 256.0)) * 2;
            s[k] = float2(S[k]) / 512.0;
        }
    }
    // Homogeneous window points P = (x w, y w, w) on the triangle's plane
    // and their lines: L[i] = P[i+1] x P[i+2] is the edge opposite vertex
    // i, and M^-1 = rows L / D, D = det(P0, P1, P2).
    float3 P[3];
    for (uint k = 0u; k < 3u; ++k) {
        P[k] = exact ? float3(s[k] * c[k].w, c[k].w)
                     : float3((c[k].x + c[k].w) * (size.x * 0.5) +
                                  origin.x * c[k].w,
                              (c[k].w - c[k].y) * (size.y * 0.5) +
                                  origin.y * c[k].w,
                              c[k].w);
    }
    float3 L[3] = {cross(P[1], P[2]), cross(P[2], P[0]), cross(P[0], P[1])};
    const float D = dot(P[0], L[0]);
    long area = 0;
    if (exact) {
        area = long(S[1].x - S[0].x) * long(S[2].y - S[0].y) -
               long(S[1].y - S[0].y) * long(S[2].x - S[0].x);
    }
    const bool degenerate = exact ? area == 0 : !(abs(D) > 0.0);
    // Ordinary (target-independent) rasterization culls a degenerate
    // triangle; tiers 2 and 3 must rasterize one after snapping. Behind the
    // eye, a degenerate triangle has no plane and no window footprint.
    if (degenerate && (!conservative || !exact)) VITREO_RETURN;
    uint tri_flags = ((flags & 2u) != 0u ? 1u : 0u) |
                     (degenerate ? 2u : 0u) | (clip_w ? 4u : 0u);
    int4 e[3];
    int4 bounds = int4(-0x40000000, -0x40000000, 0x40000000, 0x40000000);
    if (exact) {
        const int2 lo = min(S[0], min(S[1], S[2]));
        const int2 hi = max(S[0], max(S[1], S[2]));
        bounds = int4(lo, hi);
    }
    if (exact && !degenerate) {
        // A*X + B*Y + C >= 0 inside, X, Y in 1/512 pixel. An edge is top
        // (horizontal, the triangle below it) or left (the triangle to its
        // right) when its inward normal points that way (D3D11.3 3.4.2.1).
        const long sign = area > 0 ? 1 : -1;
        for (uint i = 0u; i < 3u; ++i) {
            const uint j = i == 2u ? 0u : i + 1u;
            const long A = -long(S[j].y - S[i].y) * sign;
            const long B = long(S[j].x - S[i].x) * sign;
            long C = -(A * long(S[i].x) + B * long(S[i].y));
            const bool top_left = A > 0 || (A == 0 && B > 0);
            if (!conservative && !top_left) C -= 1;
            e[i] = vitreo_pack_edge(A, B, C);
        }
    } else if (exact) {
        // A segment (or a point): its line both ways, so only pixels it
        // crosses pass, and its bounding box.
        uint a = 0u;
        uint b = 1u;
        long best = -1;
        for (uint i = 0u; i < 3u; ++i) {
            const uint j = i == 2u ? 0u : i + 1u;
            const long dx = long(S[j].x - S[i].x);
            const long dy = long(S[j].y - S[i].y);
            if (dx * dx + dy * dy > best) {
                best = dx * dx + dy * dy;
                a = i;
                b = j;
            }
        }
        const long A = -long(S[b].y - S[a].y);
        const long B = long(S[b].x - S[a].x);
        const long C = -(A * long(S[a].x) + B * long(S[a].y));
        e[0] = vitreo_pack_edge(A, B, C);
        e[1] = vitreo_pack_edge(-A, -B, -C);
        e[2] = int4(0);
    } else {
        // The oriented homogeneous lines in window pixels, scaled to
        // integers over 1/512 pixel (the clipped edges' placement is the
        // implementation's).
        const float o = D > 0.0 ? 1.0 : -1.0;
        for (uint i = 0u; i < 3u; ++i) {
            const float3 l = L[i] * o;
            const float m = max(abs(l.x), abs(l.y));
            if (!(m > 0.0)) {
                e[i] = int4(0);
                continue;
            }
            const float k = 1048576.0 / m;
            e[i] = vitreo_pack_edge(long(rint(l.x * k)), long(rint(l.y * k)),
                                    long(l.z * k * 512.0));
        }
    }
    // 1/w and z/w over window pixels: column sums of M^-1.
    float3 Q = float3(0.0, 0.0, 1.0 / c0.w);
    float3 Z = float3(0.0, 0.0, c0.z / c0.w);
    if (!degenerate) {
        Q = (L[0] + L[1] + L[2]) / D;
        Z = (c0.z * L[0] + c1.z * L[1] + c2.z * L[2]) / D;
    }
)MSL";
    }
    // The primitive's edge data: varyings, or the edge buffer.
    if (edge_kernel) {
        out += "    device int4* d = vitreo_edge_out + prim * " +
               std::to_string(kRasterEdgeBufferSlots) + "u;\n";
        out += "    d[0] = e[0];\n    d[1] = e[1];\n    d[2] = e[2];\n";
        out += "    d[3] = bounds;\n";
        out += "    d[4] = as_type<int4>(float4(Q, as_type<float>(tri_flags)));\n";
        out += "    d[5] = as_type<int4>(float4(Z, 0.0));\n";
        out += "    d[6] = e3;\n";
        out += "#undef VITREO_RECORD\n#undef VITREO_RETURN\n}\n";
        return out;
    }
    if (edge_varyings) {
        out += "    out.vitreo_edge0 = e[0];\n    out.vitreo_edge1 = e[1];\n"
               "    out.vitreo_edge2 = e[2];\n    out.vitreo_bounds = bounds;\n"
               "    out.vitreo_inverse_w = float4(Q, as_type<float>(tri_flags));\n"
               "    out.vitreo_depth = float4(Z, 0.0);\n";
        if (quad) out += "    out.vitreo_edge3 = e3;\n";
    }
    if (!tri) {
        out += raster_emulation_line_point_region_msl(source);
    } else {
        out += R"MSL(
    // The region: up to five window points, wound as the triangle faces.
    float2 poly[5];
    uint n = 0u;
    if (!degenerate && !clip_w) {
        // Each edge pushed out by h along its L1 normal; the new corner i
        // is where the lines next to it meet.
        const float o = area > 0 ? 1.0 : -1.0;
        float3 E[3];
        for (uint i = 0u; i < 3u; ++i) {
            const uint j = i == 2u ? 0u : i + 1u;
            const uint k = j == 2u ? 0u : j + 1u;
            const float3 l = cross(float3(s[j], 1.0), float3(s[k], 1.0)) * o;
            E[i] = l + float3(0.0, 0.0, h * (abs(l.x) + abs(l.y)));
        }
        bool sound = true;
        for (uint i = 0u; i < 3u; ++i) {
            const uint j = i == 2u ? 0u : i + 1u;
            const uint k = j == 2u ? 0u : j + 1u;
            const float3 v = cross(E[j], E[k]);
            if (!(abs(v.z) > 0.0)) {
                sound = false;
                break;
            }
            poly[i] = v.xy / v.z;
            if (!(distance(poly[i], s[i]) < 16.0)) sound = false;
        }
        n = sound ? 3u : 0u;
    }
    if (n == 0u) {
        // The grown bounding box (the viewport behind the eye), corners
        // clockwise on the target; reversed to face as the triangle (a
        // degenerate one back, "degenerate triangles must be considered
        // back facing").
        float2 lo;
        float2 hi;
        if (clip_w) {
            lo = origin - 1.0;
            hi = origin + size + 1.0;
        } else {
            lo = min(s[0], min(s[1], s[2])) - h;
            hi = max(s[0], max(s[1], s[2])) + h;
        }
        bool clockwise = area > 0;
        if (clip_w) clockwise = D > 0.0;
        if (degenerate) clockwise = (flags & 8u) != 0u;
        const float2 box[4] = {lo, float2(hi.x, lo.y), hi, float2(lo.x, hi.y)};
        for (uint i = 0u; i < 4u; ++i) poly[i] = box[clockwise ? i : 3u - i];
        n = 4u;
        if (!degenerate) {
            // Keep the part where the plane is in front of the eye.
            float qmax = 0.0;
            for (uint k = 0u; k < 3u; ++k) {
                if (c[k].w > 0.0) qmax = max(qmax, 1.0 / c[k].w);
            }
            const float qmin = qmax * 1.0e-6;
            float2 clipped[5];
            uint m = 0u;
            for (uint i = 0u; i < n; ++i) {
                const float2 a = poly[i];
                const float2 b = poly[i + 1u == n ? 0u : i + 1u];
                const float qa = dot(Q, float3(a, 1.0)) - qmin;
                const float qb = dot(Q, float3(b, 1.0)) - qmin;
                if (qa >= 0.0) clipped[m++] = a;
                if ((qa >= 0.0) != (qb >= 0.0) && m < 5u) {
                    clipped[m++] = mix(a, b, qa / (qa - qb));
                }
            }
            for (uint i = 0u; i < m; ++i) poly[i] = clipped[i];
            n = m;
        }
    }
    const uint fan = corner / 3u;
    const uint k = corner - fan * 3u;
    if (fan + 2u >= n) return out;
    const float2 v = poly[k == 0u ? 0u : fan + k];
    // Window point -> clip space on the triangle's plane.
    const float2 ndc = float2((v.x - origin.x) / (size.x * 0.5) - 1.0,
                              1.0 - (v.y - origin.y) / (size.y * 0.5));
    float3 beta = float3(1.0, 0.0, 0.0);
    float3 lambda = float3(1.0, 0.0, 0.0);
    if (degenerate) {
        out.position = float4(ndc, Z.z, 1.0);
    } else {
        const float3 b = float3(dot(L[0], float3(v, 1.0)),
                                dot(L[1], float3(v, 1.0)),
                                dot(L[2], float3(v, 1.0))) / D;
        const float q = b.x + b.y + b.z;
        const float w = 1.0 / q;
        beta = b * w;
        lambda = b * float3(c0.w, c1.w, c2.w);
        out.position = float4(ndc * w, dot(Z, float3(v, 1.0)) * w, w);
    }
)MSL";
    }
    for (usize i = 0; i < record.size(); ++i) {
        const auto& o = record[i];
        if (!o.delivered || o.is_position) continue;
        const std::string name = "out.locn" + std::to_string(o.register_num);
        const std::string ri = std::to_string(i) + "u";
        const RasterVarying mode =
            o.register_num < modes.size() ? modes[o.register_num]
                                          : RasterVarying::Perspective;
        if (o.component_type == 1u || o.component_type == 2u) {
            out += "    " + name + " = as_type<" + msl_type(o) +
                   ">(VITREO_RECORD(r[0], " + ri + "));\n";
        } else if (mode == RasterVarying::Constant) {
            out += "    " + name + " = VITREO_RECORD(r[0], " + ri + ");\n";
        } else {
            const char* weights =
                mode == RasterVarying::NoPerspective ? "lambda" : "beta";
            out += "    " + name + " = " + weights + ".x * VITREO_RECORD(r[0], " +
                   ri + ") + " + weights + ".y * VITREO_RECORD(r[1], " + ri +
                   ") + " + weights + ".z * VITREO_RECORD(r[2], " + ri +
                   ");\n";
        }
    }
    if (fixed_function_push) {
        out += "    const float vitreo_depth_offset = vitreo_push[" +
               std::to_string(kFixedFunctionOffset / 4u) + "u];\n";
        out += "    if (vitreo_depth_offset != 0.0) out.position.z += "
               "vitreo_depth_offset * out.position.w;\n";
    }
    if (layer.has_value()) {
        out += "    out.vitreo_layer = as_type<uint4>(VITREO_RECORD(r[0], " +
               std::to_string(layer->record_index) + "u))." +
               kLane[layer->component] + ";\n";
    }
    if (viewport.has_value()) out += "    out.vitreo_viewport = vp_raw;\n";
    if ((source.barycentrics & 1u) != 0) out += "    out.vitreo_bary = float4(beta, 0.0);\n";
    if ((source.barycentrics & 2u) != 0) out += "    out.vitreo_bary_np = float4(lambda, 0.0);\n";
    for (u32 reg = 0; reg < 32; ++reg) {
        if ((per_vertex_registers & (1u << reg)) == 0) continue;
        const auto i = per_vertex_record(reg);
        const std::string type = per_vertex_type(reg);
        for (u32 k = 0; k < 3; ++k) {
            const std::string name =
                "out.vitreo_pv" + std::to_string(reg) + "_" + std::to_string(k);
            if (!i.has_value() || k >= n_records) {
                out += "    " + name + " = " + type + "(0);\n";
                continue;
            }
            const std::string value = "VITREO_RECORD(r[" + std::to_string(k) +
                                      "], " + std::to_string(*i) + "u)";
            out += "    " + name + " = " +
                   (type == std::string("float4") ? value
                                                  : "as_type<" + type + ">(" + value + ")") +
                   ";\n";
        }
    }
    for (usize k = 0; k < clips.size(); ++k) {
        const std::string ri = std::to_string(clips[k].record_index) + "u";
        out += "    out.vitreo_clip[" + std::to_string(k) +
               "] = (beta.x * VITREO_RECORD(r[0], " + ri +
               ") + beta.y * VITREO_RECORD(r[1], " + ri +
               ") + beta.z * VITREO_RECORD(r[2], " + ri + "))." +
               kLane[clips[k].component] + ";\n";
    }
    out += "#undef VITREO_RECORD\n";
    out += "#undef VITREO_RETURN\n";
    out += "    return out;\n";
    out += "}\n";
    return out;
}

/// Pass C's vertex shader (raster_emulation_msl).
[[nodiscard]] inline std::string generate_raster_emulation_vertex_msl(
    const std::vector<GsPassthroughOutput>& outputs,
    const RasterEmulationSource& source,
    const std::array<RasterVarying, 32>& modes,
    bool fixed_function_push = false) {
    return raster_emulation_msl(outputs, source, modes, fixed_function_push, false);
}

/// The edge kernel of a pass C whose pixel shader reads the edge data from
/// the edge buffer (raster_emulation_msl).
[[nodiscard]] inline std::string generate_raster_emulation_edge_kernel_msl(
    const std::vector<GsPassthroughOutput>& outputs,
    const RasterEmulationSource& source,
    const std::array<RasterVarying, 32>& modes) {
    return raster_emulation_msl(outputs, source, modes, false, true);
}

/// The fragment function of a raster-emulated pipeline with no pixel shader
/// (a conservative depth-only pass, say): the same coverage test the
/// translated pixel shaders run (dxbc_to_spirv_raster.cpp), discarding the
/// pixels with none, and no output -- the rasterizer's depth is written as
/// it is. Entry point "main0", as a translated pixel shader's.
[[nodiscard]] inline std::string generate_raster_emulation_fragment_msl(
    const RasterEmulationOptions& raster) {
    const bool tir = raster.mode == RasterEmulationMode::TargetIndependent;
    const StandardSample* pattern =
        tir ? standard_sample_pattern(raster.samples) : nullptr;
    if (raster.mode == RasterEmulationMode::Off || (tir && pattern == nullptr)) {
        return {};
    }
    // Lines and points with no pixel shader: not generated (the caller
    // fails loudly). Every input of this function is pass C's edge data, so
    // edges_in_buffer never applies.
    if (raster.primitive != RasterPrimitive::Triangle || raster.edges_in_buffer) return {};
    const auto loc = [](u32 location) {
        return "[[user(locn" + std::to_string(location) + "), flat]]";
    };
    std::string out;
    out += "#include <metal_stdlib>\nusing namespace metal;\n\n";
    out += "// Generated by vitreo: raster-emulation coverage test with no "
           "pixel shader.\n\n";
    out += "struct vitreo_raster_in {\n"
           "    float4 position [[position]];\n";
    for (u32 i = 0; i < 3; ++i) {
        out += "    int4 vitreo_edge" + std::to_string(i) + " " +
               loc(kRasterEdgeLocation + i) + ";\n";
    }
    out += "    int4 vitreo_bounds " + loc(kRasterBoundsLocation) + ";\n";
    out += "    float4 vitreo_inverse_w " + loc(kRasterInversePlaneLocation) +
           ";\n";
    out += "    float4 vitreo_depth " + loc(kRasterDepthPlaneLocation) + ";\n";
    out += "};\n\n";
    out += R"MSL(static inline long vitreo_c(int4 e) {
    return (long(e.w) << 32) | long(uint(e.z));
}
static inline float vitreo_plane(float4 p, float x, float y) {
    return p.x * x + p.y * y + p.z;
}

struct vitreo_raster_out {
    // The rasterizer state's SampleMask, applied to the samples written.
    uint mask [[sample_mask]];
};

fragment vitreo_raster_out main0(vitreo_raster_in in [[stage_in]])
{
    const int4 e[3] = {in.vitreo_edge0, in.vitreo_edge1, in.vitreo_edge2};
    const uint flags = as_type<uint>(in.vitreo_inverse_w.w);
    const bool clip_w = (flags & 4u) != 0u;
    const bool depth_clip = (flags & 1u) != 0u;
    const float fx = floor(in.position.x);
    const float fy = floor(in.position.y);
    const int u = int(fx) * 512;
    const int v = int(fy) * 512;
)MSL";
    if (tir) {
        out += "    uint mask = 0u;\n";
        for (u32 s = 0; s < raster.samples; ++s) {
            const std::string su = std::to_string(256 + 32 * pattern[s].x);
            const std::string sv = std::to_string(256 + 32 * pattern[s].y);
            const std::string ox =
                std::to_string(0.5f + pattern[s].x / 16.0f);
            const std::string oy =
                std::to_string(0.5f + pattern[s].y / 16.0f);
            out += "    {\n";
            out += "        bool in_s = true;\n";
            out += "        for (uint i = 0u; i < 3u; ++i) {\n";
            out += "            const long value = long(e[i].x) * long(u + " + su +
                   ") + long(e[i].y) * long(v + " + sv +
                   ") + vitreo_c(e[i]);\n";
            out += "            in_s = in_s && value >= 0;\n";
            out += "        }\n";
            out += "        const float x = fx + " + ox + ";\n";
            out += "        const float y = fy + " + oy + ";\n";
            out += "        in_s = in_s && (!clip_w || vitreo_plane(in.vitreo_inverse_w, x, y) > 0.0);\n";
            out += "        const float z = vitreo_plane(in.vitreo_depth, x, y);\n";
            out += "        in_s = in_s && (!depth_clip || (z >= 0.0 && z <= 1.0));\n";
            out += "        if (in_s) mask |= " + std::to_string(1u << s) + "u;\n";
            out += "    }\n";
        }
        out += "    if (mask == 0u) discard_fragment();\n";
    } else {
        out += R"MSL(    // The pixel square grown by 1/512 pixel, against every edge at its
    // best corner, the bounding box, and the clip planes.
    const long x_lo = long(u - 1), x_hi = long(u + 513);
    const long y_lo = long(v - 1), y_hi = long(v + 513);
    bool overlap = true;
    for (uint i = 0u; i < 3u; ++i) {
        const long x = e[i].x > 0 ? x_hi : x_lo;
        const long y = e[i].y > 0 ? y_hi : y_lo;
        overlap = overlap && long(e[i].x) * x + long(e[i].y) * y + vitreo_c(e[i]) >= 0;
    }
    const int4 b = in.vitreo_bounds;
    overlap = overlap && b.x <= u + 513 && u - 1 <= b.z && b.y <= v + 513 &&
              v - 1 <= b.w;
    const float flo_x = fx - 1.0 / 512.0, fhi_x = fx + 1.0 + 1.0 / 512.0;
    const float flo_y = fy - 1.0 / 512.0, fhi_y = fy + 1.0 + 1.0 / 512.0;
    const float4 q = in.vitreo_inverse_w;
    const float q_max = vitreo_plane(q, q.x > 0.0 ? fhi_x : flo_x,
                                     q.y > 0.0 ? fhi_y : flo_y);
    const float4 d = in.vitreo_depth;
    const float z_max = vitreo_plane(d, d.x > 0.0 ? fhi_x : flo_x,
                                     d.y > 0.0 ? fhi_y : flo_y);
    const float z_min = vitreo_plane(d, d.x > 0.0 ? flo_x : fhi_x,
                                     d.y > 0.0 ? flo_y : fhi_y);
    overlap = overlap && (!clip_w || q_max > 0.0);
    overlap = overlap && (!depth_clip || (z_max >= 0.0 && z_min <= 1.0));
    if (!overlap) discard_fragment();
)MSL";
    }
    out += "    vitreo_raster_out result;\n";
    out += "    result.mask = " + std::to_string(raster.sample_mask) + "u;\n";
    out += "    return result;\n";
    out += "}\n";
    return out;
}

}  // namespace vitreo::graphics
