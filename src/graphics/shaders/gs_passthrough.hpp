// SPDX-License-Identifier: LGPL-2.1-only
/// @file gs_passthrough.hpp
/// @brief Pass C of the geometry-shader emulation: the vertex shader that
///        draws the expanded vertices.
///
/// Metal has no geometry stage, so a geometry shader is lowered to a compute
/// kernel that expands primitives into `gs_out` (stage 1, see
/// .docs/design-geometry-shader-emulation.md). Something then has to rasterise
/// what that kernel wrote, and it cannot be the guest's own vertex shader --
/// that already ran, in pass A, to produce the expansion's *input*.
///
/// This generates that shader. It is MSL text rather than a DXBC translation
/// because there is no DXBC to translate: the shader's whole contract is the
/// geometry shader's output signature, which is exactly what is passed in.
///
/// The record layout must agree with what the lowered kernel writes:
/// `gs_out[vertex * register_count + i]`, where `i` is the index of that
/// output register in ASCENDING REGISTER ORDER. The translator sorts
/// `m_gs_emit_registers` the same way; `record_order()` below is the single
/// place that ordering is expressed for this side.

#pragma once

#include "shader_types.hpp"
#include "dxbc_to_spirv.hpp"
#include "../contracts/backend_device.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// One element of the geometry shader's output signature.
struct GsPassthroughOutput {
    /// o# register number. Also the Metal `user(locnN)` index the guest's
    /// pixel shader reads it at, because the translator decorates non-position
    /// outputs with `Location = register_num`.
    u32 register_num{0};
    /// DXBC D3D_REGISTER_COMPONENT_TYPE: 1 = uint32, 2 = sint32, else float32.
    u8 component_type{3};
    /// True for the one output carrying SV_Position.
    bool is_position{false};
    /// D3D_NAME of each component that carries a rasterizer system value:
    /// 2 SV_ClipDistance, 4 SV_RenderTargetArrayIndex, 5
    /// SV_ViewportArrayIndex; 0 for an ordinary value. The register is still
    /// passed on as a varying too, since the pixel shader may read it.
    std::array<u8, 4> component_system_values{};
    /// Whether the rasterized stream outputs this register. A register only
    /// another stream of a multi-stream shader declares holds a slot of the
    /// record (the union of the streams' registers) and is not a varying.
    bool delivered{true};
};

/// Sort a signature into the order the expansion buffer stores it in.
[[nodiscard]] inline std::vector<GsPassthroughOutput> gs_record_order(
    std::vector<GsPassthroughOutput> outputs) {
    std::sort(outputs.begin(), outputs.end(),
              [](const GsPassthroughOutput& a, const GsPassthroughOutput& b) {
                  return a.register_num < b.register_num;
              });
    return outputs;
}

/// Vertices one geometry-shader INVOCATION (input primitive x GS instance)
/// can append, as independent primitives: an n-vertex triangle strip becomes
/// 3(n-2) list vertices, a line strip 2(n-1). Each invocation owns exactly
/// this many consecutive vertex slots of the expansion buffer, which is what
/// keeps the expanded primitives in D3D's order -- by input primitive, then
/// by instance -- instead of the order the GPU happened to run threads in.
[[nodiscard]] constexpr u32 gs_vertices_per_invocation(u32 output_topology,
                                                       u32 max_vertices) {
    switch (output_topology) {
    case 1: return max_vertices;                                    // points
    case 3: return max_vertices >= 2 ? 2 * (max_vertices - 1) : 0;  // lines
    case 5: return max_vertices >= 3 ? 3 * (max_vertices - 2) : 0;  // tris
    default: return 0;
    }
}

/// Triangles a TRIANGLESTRIP_ADJ draw of @p vertex_count vertices hands a
/// geometry shader: every two vertices past the first four add one; a trailing
/// odd vertex is ignored (measured: 6 and 7 vertices both give one).
[[nodiscard]] constexpr u32 triangle_strip_adjacency_count(u32 vertex_count) {
    return vertex_count >= 6 ? (vertex_count - 4) / 2 : 0;
}

/// Which strip vertex a TRIANGLESTRIP_ADJ draw hands a geometry shader as
/// `input[k]` of triangle @p i (of @p triangles). input[0], [2], [4] are the
/// triangle and [1], [3], [5] its neighbours. Measured on Windows
/// (tools/oracle_probe/d3d11_tristrip_adj_probe.c, .expected beside it);
/// Learn names the topology but not this table. Odd triangles swap the order
/// of their last two corners, the first triangle's leading neighbour is
/// vertex 1, and the last triangle's trailing neighbour is 2i+5.
[[nodiscard]] constexpr u32 triangle_strip_adjacency_vertex(u32 i, u32 k,
                                                            u32 triangles) {
    const bool last = i + 1 == triangles;
    const u32 trailing = last ? 2 * i + 5 : 2 * i + 6;
    if ((i & 1u) == 0) {
        switch (k) {
        case 0: return 2 * i;
        case 1: return i == 0 ? 1 : 2 * i - 2;
        case 2: return 2 * i + 2;
        case 3: return trailing;
        case 4: return 2 * i + 4;
        default: return 2 * i + 3;
        }
    }
    switch (k) {
    case 0: return 2 * i;
    case 1: return 2 * i + 3;
    case 2: return 2 * i + 4;
    case 3: return trailing;
    case 4: return 2 * i + 2;
    default: return 2 * i - 2;
    }
}

/// Counter-buffer layout: slots [0, kGsInvocationCountBase) are the control
/// block the host (or, for an indirect draw, the GPU resolve) writes; slot
/// kGsInvocationCountBase + s * stride + i is how many vertices invocation i
/// wrote to stream s, stride being kGsControlStreamStride's word (unused with
/// one stream). Pass C reads its stream's counts to cull the unwritten tail
/// of each invocation's region.
inline constexpr u32 kGsInvocationCountBase = 16;
/// Control words. [1]-[4] describe how a (primitive, vertex) pair becomes a
/// vertex id (see the translator's geometry prologue); the rest:
inline constexpr u32 kGsControlFlags = 0;
/// Flag bits of kGsControlFlags.
inline constexpr u32 kGsFlagInvocationLimit = 1;     ///< [5] bounds the run
inline constexpr u32 kGsFlagPrimitiveIdModulo = 2;   ///< [6] per instance
inline constexpr u32 kGsFlagPrimitiveIdTable = 4;    ///< [8] table offset
inline constexpr u32 kGsControlInvocationLimit = 5;
inline constexpr u32 kGsControlPrimitivesPerInstance = 6;
inline constexpr u32 kGsControlStreamStride = 7;
inline constexpr u32 kGsControlPrimitiveIdTable = 8;

/// The entry point name the generated shader uses.
inline constexpr const char* kGsPassthroughEntryPoint =
    "vitreo_gs_passthrough_vertex";

/// Generate the pass-C vertex shader.
///
/// It takes no vertex buffers and no input layout: every vertex is read from
/// `gs_out` by `vertex_id`, which is why pass C can always be a plain
/// point/line/triangle LIST draw with no index buffer. `gs_out` is bound as
/// the vertex stage's t0 structured SRV and the counter buffer as t1, slots
/// nothing else claims here because the guest's vertex shader does not run in
/// this pass.
///
/// The draw covers every invocation's whole region
/// (`vertices_per_invocation` each). A vertex past what its invocation wrote
/// is moved outside the clip volume, and since invocations write whole
/// primitives, the unwritten tail is whole primitives that clip away.
///
/// Returns an empty string when the signature carries no position, since a
/// draw with no clip-space output cannot rasterise and a shader that silently
/// left `position` at zero would render a degenerate primitive rather than
/// fail -- and when an invocation can write nothing at all.
///
/// With @p fixed_function_push the caller binds the translated stages' push
/// block at buffer 0 (dxbc_to_spirv.hpp kPushBlockBytes) and the shader adds
/// its clip-space depth offset to the position, as every translated vertex
/// stage does (DxbcTranslationOptions::fixed_function_push).
///
/// With @p dense_records the records are not per-invocation regions but one
/// per vertex, every one of them written -- a compute-lowered domain
/// shader's, drawn through the tessellator's index list -- so nothing is
/// culled (and @p vertices_per_invocation is unused).
[[nodiscard]] inline std::string generate_gs_passthrough_vertex_msl(
    const std::vector<GsPassthroughOutput>& outputs,
    u32 vertices_per_invocation, bool fixed_function_push = false,
    bool dense_records = false) {
    const auto record = gs_record_order(outputs);
    if (dense_records) vertices_per_invocation = 0;
    if (record.empty() || (vertices_per_invocation == 0 && !dense_records)) {
        return {};
    }
    if (std::none_of(record.begin(), record.end(),
                     [](const GsPassthroughOutput& o) {
                         return o.is_position && o.delivered;
                     })) {
        return {};
    }

    // Metal makes integer varyings flat and offers no alternative, so they
    // take no interpolation qualifier -- matching what SPIRV-Cross emits for
    // the guest's pixel shader, which this has to link against.
    const auto msl_type = [](const GsPassthroughOutput& o) -> const char* {
        if (o.component_type == 1u) return "uint4";
        if (o.component_type == 2u) return "int4";
        return "float4";
    };
    // Rasterizer system values, by (record index, component).
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

    std::string out;
    out += "#include <metal_stdlib>\n";
    out += "using namespace metal;\n\n";
    out += "// Generated by vitreo: geometry-shader expansion, pass C.\n";
    out += "// Reads the vertices the expansion kernel appended to gs_out.\n\n";
    out += "struct vitreo_gs_passthrough_out {\n";
    for (const auto& o : record) {
        if (!o.delivered) continue;
        if (o.is_position) {
            out += "    float4 position [[position]];\n";
        } else {
            out += std::string("    ") + msl_type(o) + " locn" +
                   std::to_string(o.register_num) + " [[user(locn" +
                   std::to_string(o.register_num) + ")]];\n";
        }
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
    out += "};\n\n";

    const std::string capacity = std::to_string(vertices_per_invocation) + "u";
    out += std::string("vertex vitreo_gs_passthrough_out ") +
           kGsPassthroughEntryPoint + "(\n";
    out += "    uint vitreo_vertex_id [[vertex_id]],\n";
    out += "    const device float4* vitreo_gs_out [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n";
    out += "    const device uint* vitreo_gs_counts [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE + 1) + ")]]";
    if (fixed_function_push) {
        // The translated stages' push block (kPushBlockBytes, buffer 0): its
        // clip-space depth offset carries a UNORM depth bias's constant part
        // (dxbc_to_spirv.hpp kFixedFunctionOffset), as for any vertex stage.
        out += ",\n    constant float* vitreo_push [[buffer(0)]]";
    }
    out += ")\n";
    out += "{\n";
    out += "    vitreo_gs_passthrough_out out = {};\n";
    if (vertices_per_invocation != 0) {
        out += "    const uint invocation = vitreo_vertex_id / " + capacity +
               ";\n";
        out += "    if (vitreo_vertex_id - invocation * " + capacity +
               " >= vitreo_gs_counts[" +
               std::to_string(kGsInvocationCountBase) +
               "u + invocation]) {\n";
        out += "        // Not written by its invocation: outside every clip "
               "plane.\n";
        out += "        out.position = float4(2.0, 2.0, 2.0, 1.0);\n";
        out += "        return out;\n";
        out += "    }\n";
    }
    out += "    const uint base = vitreo_vertex_id * " +
           std::to_string(record.size()) + "u;\n";
    const auto source_of = [](usize i) {
        return "vitreo_gs_out[base + " + std::to_string(i) + "u]";
    };
    for (usize i = 0; i < record.size(); ++i) {
        const auto& o = record[i];
        if (!o.delivered) continue;
        const std::string source = source_of(i);
        if (o.is_position) {
            out += "    out.position = " + source + ";\n";
        } else if (o.component_type == 1u || o.component_type == 2u) {
            // The buffer is untyped storage: an integer output was stored by
            // reinterpreting its bits, so it is read back the same way.
            out += std::string("    out.locn") +
                   std::to_string(o.register_num) + " = as_type<" +
                   msl_type(o) + ">(" + source + ");\n";
        } else {
            out += "    out.locn" + std::to_string(o.register_num) + " = " +
                   source + ";\n";
        }
    }
    if (fixed_function_push) {
        out += "    const float vitreo_depth_offset = vitreo_push[" +
               std::to_string(kFixedFunctionOffset / 4u) + "u];\n";
        out += "    if (vitreo_depth_offset != 0.0) out.position.z += "
               "vitreo_depth_offset * out.position.w;\n";
    }
    static constexpr const char* kLane[4] = {"x", "y", "z", "w"};
    if (layer.has_value()) {
        out += "    out.vitreo_layer = as_type<uint4>(" +
               source_of(layer->record_index) + ")." +
               kLane[layer->component] + ";\n";
    }
    if (viewport.has_value()) {
        out += "    out.vitreo_viewport = as_type<uint4>(" +
               source_of(viewport->record_index) + ")." +
               kLane[viewport->component] + ";\n";
    }
    for (usize k = 0; k < clips.size(); ++k) {
        out += "    out.vitreo_clip[" + std::to_string(k) + "] = " +
               source_of(clips[k].record_index) + "." +
               kLane[clips[k].component] + ";\n";
    }
    out += "    return out;\n";
    out += "}\n";
    return out;
}

/// Lines as D3D rasterizes them when the rasterizer state asks for more than
/// aliased lines (Learn D3D11_RASTERIZER_DESC: MultisampleEnable selects
/// quadrilateral lines, AntialiasedLineEnable alone alpha-antialiased ones;
/// on 1x targets too, measured on WARP, d3d11_a07_rs_probe `R7`). Metal draws
/// only aliased lines, so pass C draws each line as the rectangle around it,
/// in two triangles:
///   - quadrilateral (D3D11.3 3.4.5): 1.4 pixels wide, ends at the endpoints;
///   - alpha-antialiased (3.4.4): WARP rasterizes a rectangle 4 pixels wide
///     (half-width 2) and multiplies o0.a by 0.675 - 0.45 d, d the pixel
///     centre's distance from the line in pixels, clamped to [0, 1]
///     (measured; every cell of d3d11_a07_rs_probe `R7 lines alpha-AA` and
///     d3d11_view_line_probe [line] follows it). The shader writes the
///     line's equation in window pixels, which the pixel-shader variant
///     evaluates at the pixel (DxbcTranslationOptions::line_coverage_alpha):
///     interpolating a distance from the snapped corners lands a unit off.
/// Both are ordinary triangles: the top-left rule, per-sample coverage on
/// MSAA targets, and attributes interpolated along the line. A wireframe
/// triangle's edges are lines too (15.13): each triangle becomes its three
/// edge rectangles, v0v1, v1v2, v2v0, after the triangle's own facing and
/// culling.
struct LineQuadSource {
    /// Records per vertex found through 32-bit record ids at t1,
    /// `vertices_per_primitive` per primitive (a vertex capture) -- or, when
    /// nonzero, consecutive slots of per-invocation regions of this many
    /// vertices whose written counts are at t1 (a geometry expansion, laid
    /// out as for generate_gs_passthrough_vertex_msl).
    u32 vertices_per_invocation{0};
    /// 2: lines; 4: lines with adjacency (the line is vertices 1 and 2);
    /// 3: triangles drawn as their edges.
    u32 vertices_per_primitive{2};
};

/// t2 of a line-quad pass C: the per-draw state its shader reads.
struct LineQuadParams {
    /// 0.7 quadrilateral, 2.0 alpha-antialiased.
    float half_width{0.7f};
    /// The rasterizer state's FrontCounterClockwise. Lines face front; a
    /// wireframe edge faces as its triangle does.
    u32 front_counter_clockwise{0};
    /// Wireframe triangles only: D3D11_CULL_MODE - 1 (0 none, 1 front, 2
    /// back), applied to the triangle before its edges are drawn.
    u32 cull{0};
    /// Primitives the draw holds: pass C draws exactly 6 (18 for wireframe)
    /// vertices per primitive.
    u32 primitives{0};
    /// Each viewport's (width, height, TopLeftX, TopLeftY): NDC to window
    /// pixels.
    std::array<std::array<float, 4>, 16> viewports{};
};
static_assert(sizeof(LineQuadParams) == 16 + 16 * 16);

/// Metal triangles per D3D primitive a line-quad pass C draws: two per line,
/// six per wireframe triangle. SV_PrimitiveID is Metal's primitive id divided
/// by it (DxbcTranslationOptions::line_quad_primitive_divisor).
[[nodiscard]] constexpr u32 line_quad_triangles_per_primitive(
    u32 vertices_per_primitive) {
    return vertices_per_primitive == 3 ? 6u : 2u;
}

/// Generate a line-quad pass C vertex shader over @p outputs (as for
/// generate_gs_passthrough_vertex_msl). Draw 6 vertices per line and 18 per
/// wireframe triangle, non-indexed; empty when the records hold no
/// position.
[[nodiscard]] inline std::string generate_line_quad_vertex_msl(
    const std::vector<GsPassthroughOutput>& outputs,
    const LineQuadSource& source, bool fixed_function_push = false) {
    const auto record = gs_record_order(outputs);
    const u32 vpp = source.vertices_per_primitive;
    if (record.empty() || (vpp != 2 && vpp != 3 && vpp != 4)) return {};
    usize position_index = record.size();
    for (usize i = 0; i < record.size(); ++i) {
        if (record[i].is_position && record[i].delivered) position_index = i;
    }
    if (position_index == record.size()) return {};
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
    const bool triangles = vpp == 3;
    // The line's endpoints within its primitive; a triangle's edge picks
    // them at run time.
    const u32 first = vpp == 4 ? 1u : 0u;
    // The vertex whose record carries the primitive's viewport and
    // render-target array index: the line's first, the triangle's first.
    const u32 provoking = triangles ? 0u : first;
    const std::string stride = std::to_string(record.size()) + "u";
    const std::string n = std::to_string(vpp) + "u";
    static constexpr const char* kLane[4] = {"x", "y", "z", "w"};

    std::string out;
    out += "#include <metal_stdlib>\n";
    out += "using namespace metal;\n\n";
    out += "// Generated by vitreo: lines drawn as D3D's rectangles "
           "(line-quad pass C).\n\n";
    out += "struct vitreo_line_quad_params {\n"
           "    float half_width;\n"
           "    uint front_ccw;\n"
           "    uint cull;\n"
           "    uint primitives;\n"
           "    float4 viewport[16];\n"
           "};\n\n";
    out += "struct vitreo_gs_passthrough_out {\n";
    for (const auto& o : record) {
        if (!o.delivered) continue;
        if (o.is_position) {
            out += "    float4 position [[position]];\n";
        } else {
            out += std::string("    ") + msl_type(o) + " locn" +
                   std::to_string(o.register_num) + " [[user(locn" +
                   std::to_string(o.register_num) + ")]];\n";
        }
    }
    out += "    float3 vitreo_line_equation [[user(locn" +
           std::to_string(kLineDistanceLocation) + ")]];\n";
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
    out += "};\n\n";
    out += std::string("vertex vitreo_gs_passthrough_out ") +
           kGsPassthroughEntryPoint + "(\n";
    out += "    uint vitreo_vertex_id [[vertex_id]],\n";
    out += "    const device float4* vitreo_gs_out [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n";
    out += "    const device uint* vitreo_gs_counts [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE + 1) + ")]],\n";
    out += "    constant vitreo_line_quad_params& vitreo_line [[buffer(" +
           std::to_string(BACKEND_SRV_BUFFER_BASE + 2) + ")]]";
    if (fixed_function_push) {
        out += ",\n    constant float* vitreo_push [[buffer(0)]]";
    }
    out += ")\n{\n";
    out += "    vitreo_gs_passthrough_out out = {};\n";
    // Anything not drawn sits outside every clip plane.
    out += "    out.position = float4(2.0, 2.0, 2.0, 1.0);\n";
    out += "    const uint quad = vitreo_vertex_id / 6u;\n";
    out += "    const uint corner = vitreo_vertex_id - quad * 6u;\n";
    if (triangles) {
        out += "    const uint prim = quad / 3u;\n";
        out += "    const uint edge = quad - prim * 3u;\n";
    } else {
        out += "    const uint prim = quad;\n";
    }
    out += "    if (prim >= vitreo_line.primitives) return out;\n";
    out += "    uint r[" + std::to_string(vpp) + "];\n";
    if (source.vertices_per_invocation != 0) {
        const std::string capacity =
            std::to_string(source.vertices_per_invocation) + "u";
        out += "    const uint slot0 = prim * " + n + ";\n";
        out += "    const uint invocation = slot0 / " + capacity + ";\n";
        out += "    // A primitive its invocation did not write is not "
               "drawn.\n";
        out += "    if (slot0 - invocation * " + capacity + " + " + n +
               " > vitreo_gs_counts[" +
               std::to_string(kGsInvocationCountBase) +
               "u + invocation]) return out;\n";
        out += "    for (uint k = 0u; k < " + n + "; ++k) r[k] = slot0 + k;\n";
    } else {
        out += "    for (uint k = 0u; k < " + n +
               "; ++k) r[k] = vitreo_gs_counts[prim * " + n + " + k];\n";
    }
    const std::string pos = std::to_string(position_index) + "u";
    out += "#define VITREO_RECORD(v, i) vitreo_gs_out[(v) * " + stride +
           " + (i)]\n";
    if (triangles) {
        out += "    const uint ra = r[edge];\n";
        out += "    const uint rb = r[edge == 2u ? 0u : edge + 1u];\n";
        // D3D11.3 15.13: a wireframe triangle is culled by its facing and
        // its edges face as it does. det[x y w] > 0 is counter-clockwise
        // on the render target for w > 0 (homogeneous, so it holds before
        // clipping).
        out += "    const float4 p0 = VITREO_RECORD(r[0], " + pos + ");\n";
        out += "    const float4 p1 = VITREO_RECORD(r[1], " + pos + ");\n";
        out += "    const float4 p2 = VITREO_RECORD(r[2], " + pos + ");\n";
        out += "    const float det = determinant(float3x3(p0.xyw, p1.xyw, "
               "p2.xyw));\n";
        out += "    if (!(det != 0.0)) return out;\n";
        out += "    const bool front = (det > 0.0) == "
               "(vitreo_line.front_ccw != 0u);\n";
        out += "    if ((vitreo_line.cull == 1u && front) || "
               "(vitreo_line.cull == 2u && !front)) return out;\n";
    } else {
        out += "    const uint ra = r[" + std::to_string(first) + "u];\n";
        out += "    const uint rb = r[" + std::to_string(first + 1) + "u];\n";
        out += "    const bool front = true;\n";
    }
    // Clip the segment to w >= eps (D3D clips every primitive to 0 < w);
    // the rectangle is built in screen space, which needs w > 0 at both
    // ends. The other planes clip the rectangle's triangles.
    out += "    const float4 pa = VITREO_RECORD(ra, " + pos + ");\n";
    out += "    const float4 pb = VITREO_RECORD(rb, " + pos + ");\n";
    out += "    const float eps = 1.0e-6;\n";
    out += "    if (pa.w < eps && pb.w < eps) return out;\n";
    out += "    float ta = 0.0;\n";
    out += "    float tb = 1.0;\n";
    out += "    if (pa.w < eps) ta = (eps - pa.w) / (pb.w - pa.w);\n";
    out += "    if (pb.w < eps) tb = (eps - pa.w) / (pb.w - pa.w);\n";
    out += "    const float4 ca = mix(pa, pb, ta);\n";
    out += "    const float4 cb = mix(pa, pb, tb);\n";
    const std::string provoking_record =
        triangles ? std::string("r[0]") : "r[" + std::to_string(provoking) +
                                              "u]";
    if (viewport.has_value()) {
        out += "    const uint vp_raw = as_type<uint4>(VITREO_RECORD(" +
               provoking_record + ", " +
               std::to_string(viewport->record_index) + "u))." +
               kLane[viewport->component] + ";\n";
        out += "    const uint vp_index = min(vp_raw, 15u);\n";
    } else {
        out += "    const uint vp_index = 0u;\n";
    }
    out += "    const float2 size = vitreo_line.viewport[vp_index].xy;\n";
    out += "    const float2 origin = vitreo_line.viewport[vp_index].zw;\n";
    // Window pixels, y down, as the pixel shader's position.
    out += "    const float2 sa = (float2(ca.x, -ca.y) / ca.w + 1.0) * size * "
           "0.5 + origin;\n";
    out += "    const float2 sb = (float2(cb.x, -cb.y) / cb.w + 1.0) * size * "
           "0.5 + origin;\n";
    out += "    const float2 d = sb - sa;\n";
    out += "    const float len = length(d);\n";
    out += "    if (!(len > 0.0)) return out;\n";
    out += "    const float2 normal = float2(-d.y, d.x) / len;\n";
    // Corners a-, a+, b-, b+ (a/b the endpoint, +-normal the side). With
    // y down, (a-, b-, a+), (a+, b-, b+) is clockwise on the render target
    // and (a-, a+, b-), (a+, b+, b-) counter-clockwise; each triangle
    // starts at an `a` corner, the line's first vertex, which provokes flat
    // attributes. The winding makes the rectangle face as the primitive.
    out += "    const bool ccw = front == (vitreo_line.front_ccw != 0u);\n";
    out += "    const uint cw_end[6] = {0u, 1u, 0u, 0u, 1u, 1u};\n";
    out += "    const float cw_side[6] = {-1.0, -1.0, 1.0, 1.0, -1.0, 1.0};\n";
    out += "    const uint ccw_end[6] = {0u, 0u, 1u, 0u, 1u, 1u};\n";
    out += "    const float ccw_side[6] = {-1.0, 1.0, -1.0, 1.0, 1.0, -1.0};\n";
    out += "    const uint end = ccw ? ccw_end[corner] : cw_end[corner];\n";
    out += "    const float side = ccw ? ccw_side[corner] : cw_side[corner];\n";
    out += "    const float t = end != 0u ? tb : ta;\n";
    out += "    const float4 base = end != 0u ? cb : ca;\n";
    out += "    const float2 offset = normal * (side * vitreo_line.half_width);\n";
    out += "    out.position = base + float4(float2(offset.x, -offset.y) * "
           "2.0 / size * base.w, 0.0, 0.0);\n";
    out += "    out.vitreo_line_equation = float3(normal, -dot(normal, sa));\n";
    const auto lerp_of = [&](usize i) {
        return "mix(VITREO_RECORD(ra, " + std::to_string(i) +
               "u), VITREO_RECORD(rb, " + std::to_string(i) + "u), t)";
    };
    for (usize i = 0; i < record.size(); ++i) {
        const auto& o = record[i];
        if (!o.delivered || o.is_position) continue;
        const std::string name = "out.locn" + std::to_string(o.register_num);
        if (o.component_type == 1u || o.component_type == 2u) {
            // Integer varyings are flat: the endpoint's own bits.
            out += "    " + name + " = as_type<" + msl_type(o) +
                   ">(VITREO_RECORD(end != 0u ? rb : ra, " +
                   std::to_string(i) + "u));\n";
        } else {
            out += "    " + name + " = " + lerp_of(i) + ";\n";
        }
    }
    if (fixed_function_push) {
        out += "    const float vitreo_depth_offset = vitreo_push[" +
               std::to_string(kFixedFunctionOffset / 4u) + "u];\n";
        out += "    if (vitreo_depth_offset != 0.0) out.position.z += "
               "vitreo_depth_offset * out.position.w;\n";
    }
    if (layer.has_value()) {
        out += "    out.vitreo_layer = as_type<uint4>(VITREO_RECORD(" +
               provoking_record + ", " + std::to_string(layer->record_index) +
               "u))." + kLane[layer->component] + ";\n";
    }
    if (viewport.has_value()) {
        out += "    out.vitreo_viewport = vp_raw;\n";
    }
    for (usize k = 0; k < clips.size(); ++k) {
        out += "    out.vitreo_clip[" + std::to_string(k) + "] = " +
               lerp_of(clips[k].record_index) + "." +
               kLane[clips[k].component] + ";\n";
    }
    out += "#undef VITREO_RECORD\n";
    out += "    return out;\n";
    out += "}\n";
    return out;
}

}  // namespace vitreo::graphics
