// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_raster.cpp
/// @brief Pixel-shader side of target-independent and conservative
///        rasterization (DxbcTranslationOptions::raster_emulation).
///
/// Pass C (raster_emulation.hpp) rasterizes a region holding every
/// candidate pixel's centre and hands each fragment its triangle's 16.8
/// snapped edge functions. The prologue here decides the D3D coverage:
///
///   - target-independent rasterization (D3D11.3 3.5.6.2): sample s of the
///     forced standard pattern is covered when every edge function is >= 0
///     there (the top-left rule is folded into the edges), 1/w > 0 for a
///     triangle crossing w = 0, and 0 <= z/w <= 1 with depth clipping;
///     "Pixel Shader input coverage to the shader sees the primitive's
///     coverage of the ForcedSampleCount sample pattern";
///   - conservative rasterization: the pixel square grown by the 1/512
///     snapping slack overlaps the snapped triangle (separating axes: the
///     three edges at the square's best corner each, and the bounding box),
///     and then "all samples are covered" -- the first n bits, ANDed with
///     the SampleMask state; InnerCoverage when the grown square lies inside
///     every edge (Rådström cancellation: the square is then inside the
///     unsnapped triangle, so there is no false positive, and a pixel inside
///     the triangle shrunk by 1/256 is never missed).
///
/// A pixel with no coverage is demoted to a helper invocation: derivatives
/// keep working and its side effects are dropped, as for a pixel the
/// rasterizer never covered.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

void DxbcTranslator::declare_raster_emulation_inputs() {
    const auto flat_input = [&](u32 type, u32 location, const char* name) {
        const u32 var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Input, type),
            SpvStorageClass::Input);
        m_builder.emit_name(var, name);
        m_builder.emit_decorate(var, SpvDecoration::Location, location);
        m_builder.emit_decorate(var, SpvDecoration::Flat);
        return var;
    };
    const RasterEmulationOptions& re = m_options.raster_emulation;
    static constexpr const char* kEdgeNames[4] = {
        "vitreo_edge0", "vitreo_edge1", "vitreo_edge2", "vitreo_edge3"};
    if (re.edges_in_buffer) {
        // Private copies the prologue fills from the edge buffer
        // (RasterEmulationOptions::edges_in_buffer).
        const auto private_copy = [&](u32 type, const char* name) {
            const u32 var = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Private, type),
                SpvStorageClass::Private);
            m_builder.emit_name(var, name);
            return var;
        };
        for (u32 i = 0; i < 3; ++i) m_re_edges[i] = private_copy(m_type_ivec4, kEdgeNames[i]);
        m_re_edge3 = private_copy(m_type_ivec4, kEdgeNames[3]);
        m_re_bounds = private_copy(m_type_ivec4, "vitreo_bounds");
        m_re_inverse_w = private_copy(m_type_vec4, "vitreo_inverse_w");
        m_re_depth = private_copy(m_type_vec4, "vitreo_depth");
        const u32 array_ty = m_builder.emit_type_runtime_array(m_type_ivec4);
        m_builder.emit_decorate(array_ty, SpvDecoration::ArrayStride, 16u);
        const u32 struct_ty = m_builder.emit_type_struct({array_ty});
        m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
        m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::NonWritable);
        m_re_edge_buffer = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, struct_ty),
            SpvStorageClass::StorageBuffer);
        m_builder.emit_name(m_re_edge_buffer, "vitreo_raster_edge_data");
        m_builder.emit_decorate(m_re_edge_buffer, SpvDecoration::DescriptorSet,
                                SPIRV_STATISTICS_DESCRIPTOR_SET);
        m_builder.emit_decorate(m_re_edge_buffer, SpvDecoration::Binding, 0u);
        // Metal's primitive id: the shader's own SV_PrimitiveID variable when
        // it has one (Metal takes one [[primitive_id]]).
        m_re_primitive_id = 0;
        for (const auto& input : m_container.input_signature) {
            if (input.system_value_type != 7u) continue;
            const auto found = m_input_scalar_builtins.find(input.register_num);
            if (found != m_input_scalar_builtins.end() && !found->second.empty()) {
                m_re_primitive_id = found->second.front().var;
            }
        }
        if (m_re_primitive_id == 0) {
            m_builder.emit_capability(SpvCapability::Geometry);
            m_re_primitive_id = m_builder.emit_variable(m_type_ptr_input_uint,
                                                        SpvStorageClass::Input);
            m_builder.emit_name(m_re_primitive_id, "vitreo_raster_primitive");
            m_builder.emit_decorate(m_re_primitive_id, SpvDecoration::BuiltIn,
                                    static_cast<u32>(SpvBuiltIn::PrimitiveId));
            m_builder.emit_decorate(m_re_primitive_id, SpvDecoration::Flat);
            m_re_primitive_id_declared = m_re_primitive_id;
        }
    } else {
        for (u32 i = 0; i < 3; ++i) {
            m_re_edges[i] =
                flat_input(m_type_ivec4, kRasterEdgeLocation + i, kEdgeNames[i]);
        }
        if (re.primitive == RasterPrimitive::Quad) {
            m_re_edge3 = flat_input(m_type_ivec4, kRasterEdge3Location, kEdgeNames[3]);
        }
        m_re_bounds =
            flat_input(m_type_ivec4, kRasterBoundsLocation, "vitreo_bounds");
        m_re_inverse_w = flat_input(m_type_vec4, kRasterInversePlaneLocation,
                                    "vitreo_inverse_w");
        m_re_depth =
            flat_input(m_type_vec4, kRasterDepthPlaneLocation, "vitreo_depth");
    }
    if (const auto position = m_input_vars.find(m_frag_coord_register);
        m_frag_coord_register != ~0u && position != m_input_vars.end()) {
        m_re_frag_coord = position->second;
    } else {
        m_re_frag_coord = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_vec4),
            SpvStorageClass::Input);
        m_builder.emit_name(m_re_frag_coord, "vitreo_raster_position");
        m_builder.emit_decorate(m_re_frag_coord, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::FragCoord));
        m_re_frag_coord_declared = m_re_frag_coord;
    }
    const auto private_var = [&](u32 type, const char* name) {
        const u32 var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, type),
            SpvStorageClass::Private);
        m_builder.emit_name(var, name);
        return var;
    };
    m_re_mask = private_var(m_type_uint, "vitreo_raster_coverage");
    m_re_inner = private_var(m_type_uint, "vitreo_inner_coverage");
    m_re_centroid_offset =
        private_var(m_type_vec2, "vitreo_raster_centroid_offset");
}

u32 DxbcTranslator::raster_sample_offset(u32 index) {
    // SPIRV-Cross turns a centre-relative SPIR-V offset into MSL's
    // corner-relative interpolate_at_offset by adding 0.4375; the missing
    // 1/16 is added back here, as for eval_snapped (op_query).
    const RasterEmulationOptions& re = m_options.raster_emulation;
    const StandardSample* pattern = standard_sample_pattern(re.samples);
    const u32 sixteenth = m_builder.emit_constant_float(m_type_float, 1.0f / 16.0f);
    u32 offset = m_builder.emit_composite_construct(m_type_vec2,
                                                    {sixteenth, sixteenth});
    if (pattern == nullptr) return offset;
    for (u32 s = 0; s < re.samples; ++s) {
        const u32 at = m_builder.emit_constant_composite(
            m_type_vec2,
            {m_builder.emit_constant_float(m_type_float,
                                           (pattern[s].x + 1) / 16.0f),
             m_builder.emit_constant_float(m_type_float,
                                           (pattern[s].y + 1) / 16.0f)});
        const u32 is = m_builder.emit_integer_compare(
            SpvOp::OpIEqual, m_type_bool, index,
            m_builder.emit_constant_int(m_type_int, s));
        offset = m_builder.emit_select(
            m_type_vec2,
            m_builder.emit_composite_construct(m_type_bvec2, {is, is}), at,
            offset);
    }
    return offset;
}

bool DxbcTranslator::emit_raster_emulation_prologue() {
    const RasterEmulationOptions& re = m_options.raster_emulation;
    const bool tir = re.mode == RasterEmulationMode::TargetIndependent;
    const StandardSample* pattern =
        tir ? standard_sample_pattern(re.samples) : nullptr;
    if (tir && pattern == nullptr) {
        return fail("raster emulation: no standard pattern of " +
                    std::to_string(re.samples) + " samples");
    }
    if (!tir && (re.samples == 0 || re.samples > 32)) {
        return fail("raster emulation: bad conservative sample count");
    }
    m_builder.emit_capability(SpvCapability::Int64);
    const u32 t_long = m_builder.emit_type_int(64, true);
    const u32 t_bool = m_type_bool;
    const auto c_int = [&](i32 v) {
        return m_builder.emit_constant_int(m_type_int, static_cast<u32>(v));
    };
    const auto c_uint = [&](u32 v) {
        return m_builder.emit_constant_int(m_type_uint, v);
    };
    const auto c_float = [&](float v) {
        return m_builder.emit_constant_float(m_type_float, v);
    };
    const auto widen = [&](u32 int_value) {
        m_builder.set_section(SpirvBuilder::Section::Functions);
        const u32 id = m_builder.allocate_id();
        m_builder.emit(SpvOp::OpSConvert, {t_long, id, int_value});
        return id;
    };
    const auto l_add = [&](u32 a, u32 b) { return m_builder.emit_iadd(t_long, a, b); };
    const auto l_mul = [&](u32 a, u32 b) { return m_builder.emit_imul(t_long, a, b); };
    const auto l_ge0 = [&](u32 a) {
        return m_builder.emit_integer_compare(SpvOp::OpSGreaterThanEqual,
                                              t_bool, a, widen(c_int(0)));
    };
    const auto all_of = [&](u32 a, u32 b) {
        return m_builder.emit_logical(SpvOp::OpLogicalAnd, t_bool, a, b);
    };
    const auto any_of = [&](u32 a, u32 b) {
        return m_builder.emit_logical(SpvOp::OpLogicalOr, t_bool, a, b);
    };
    const auto lane = [&](u32 type, u32 vector, u32 index) {
        return m_builder.emit_composite_extract(type, vector, {index});
    };

    // The edge buffer: this primitive's kRasterEdgeBufferSlots int4 at
    // Metal primitive id / 3 (pass C draws three Metal triangles per D3D
    // primitive, kRasterTrianglesPerPrimitive).
    if (re.edges_in_buffer) {
        const u32 metal_prim = m_builder.emit_load(m_type_uint, m_re_primitive_id);
        const u32 base = m_builder.emit_imul(
            m_type_uint, m_builder.emit_udiv(m_type_uint, metal_prim, c_uint(3u)),
            c_uint(8u));
        const u32 slot_ptr =
            m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_ivec4);
        const auto slot = [&](u32 k) {
            return m_builder.emit_load(
                m_type_ivec4,
                m_builder.emit_access_chain(slot_ptr, m_re_edge_buffer,
                                            {m_const_u0, m_builder.emit_iadd(m_type_uint, base, c_uint(k))}));
        };
        for (u32 i = 0; i < 3; ++i) m_builder.emit_store(m_re_edges[i], slot(i));
        m_builder.emit_store(m_re_bounds, slot(3));
        m_builder.emit_store(m_re_inverse_w, m_builder.emit_bitcast(m_type_vec4, slot(4)));
        m_builder.emit_store(m_re_depth, m_builder.emit_bitcast(m_type_vec4, slot(5)));
        m_builder.emit_store(m_re_edge3, slot(6));
    }
    const bool quad = re.primitive == RasterPrimitive::Quad;
    const bool aliased_line = re.primitive == RasterPrimitive::AliasedLine;
    if (aliased_line && !tir) {
        return fail("raster emulation: an aliased line is drawn conservatively as a quad");
    }
    const u32 edge_count = quad ? 4u : 3u;

    // The primitive's edges: A, B and the 64-bit C.
    struct Edge {
        u32 a, b, c;
    };
    std::array<Edge, 4> edges{};
    for (u32 i = 0; i < edge_count; ++i) {
        const u32 packed = m_builder.emit_load(m_type_ivec4, i < 3 ? m_re_edges[i] : m_re_edge3);
        const u32 lo = m_builder.emit_shift_right_logical(
            t_long,
            m_builder.emit_shift_left_logical(
                t_long, widen(lane(m_type_int, packed, 2)), c_uint(32)),
            c_uint(32));
        const u32 hi = m_builder.emit_shift_left_logical(
            t_long, widen(lane(m_type_int, packed, 3)), c_uint(32));
        edges[i] = {widen(lane(m_type_int, packed, 0)),
                    widen(lane(m_type_int, packed, 1)),
                    m_builder.emit_bitwise_or(t_long, hi, lo)};
    }
    const u32 inverse_w = m_builder.emit_load(m_type_vec4, m_re_inverse_w);
    const u32 depth = m_builder.emit_load(m_type_vec4, m_re_depth);
    const u32 tri_flags = m_builder.emit_bitcast(
        m_type_uint, lane(m_type_float, inverse_w, 3));
    const auto flag = [&](u32 bit) {
        return m_builder.emit_inotequal(
            t_bool, m_builder.emit_bitwise_and(m_type_uint, tri_flags, c_uint(bit)),
            m_const_u0);
    };
    const u32 clip_w = flag(kRasterTriangleClipW);
    const u32 depth_clip = flag(kRasterTriangleDepthClip);
    const u32 degenerate = flag(kRasterTriangleDegenerate);
    // a*x + b*y + c of a window-pixel plane.
    const auto plane = [&](u32 p, u32 x, u32 y) {
        return m_builder.emit_fadd(
            m_type_float,
            m_builder.emit_fadd(
                m_type_float,
                m_builder.emit_fmul(m_type_float, lane(m_type_float, p, 0), x),
                m_builder.emit_fmul(m_type_float, lane(m_type_float, p, 1), y)),
            lane(m_type_float, p, 2));
    };
    const auto f_gt0 = [&](u32 v) {
        return m_builder.emit_ford_lt(t_bool, c_float(0.0f), v);
    };
    const auto f_le = [&](u32 a, u32 b) {  // a <= b
        return m_builder.emit_ford_ge(t_bool, b, a);
    };

    // The pixel: its integer window coordinates.
    const u32 position = m_builder.emit_load(m_type_vec4, m_re_frag_coord);
    const u32 fx = m_builder.emit_ext_inst(m_type_float, m_glsl_std450,
                                           kGlslStd450Floor,
                                           {lane(m_type_float, position, 0)});
    const u32 fy = m_builder.emit_ext_inst(m_type_float, m_glsl_std450,
                                           kGlslStd450Floor,
                                           {lane(m_type_float, position, 1)});
    const u32 px = m_builder.emit_convert_f_to_s(m_type_int, fx);
    const u32 py = m_builder.emit_convert_f_to_s(m_type_int, fy);
    const u32 u_base = m_builder.emit_imul(m_type_int, px, c_int(512));
    const u32 v_base = m_builder.emit_imul(m_type_int, py, c_int(512));
    const auto edge_at = [&](const Edge& e, u32 x_long, u32 y_long) {
        return l_add(l_add(l_mul(e.a, x_long), l_mul(e.b, y_long)), e.c);
    };

    u32 mask = 0;
    u32 covered = 0;
    if (tir && aliased_line) {
        // D3D11.3 3.4.3: the line covers the pixel when it exits the pixel's
        // diamond travelling from A to B. Relative to the centre, 1/512
        // pixel, the x-major diamond is -x-y < 256, x-y < 256 (the upper
        // edges and the top, left and right corners excluded), -x+y <= 256,
        // x+y <= 256 (the lower edges and the bottom corner included); a
        // y-major one also includes the right corner (256, 0). The segment
        // exits the (convex) diamond iff it meets it and B is outside: the
        // meeting is a clip of t in [0, 1] by the four half-planes, exact in
        // 64-bit fractions. A covered pixel hits every sample (15.14.1).
        const u32 packed = m_builder.emit_load(m_type_ivec4, m_re_edges[0]);
        const u32 cx = widen(m_builder.emit_iadd(m_type_int, u_base, c_int(256)));
        const u32 cy = widen(m_builder.emit_iadd(m_type_int, v_base, c_int(256)));
        const auto l_sub = [&](u32 a, u32 b) { return m_builder.emit_isub(t_long, a, b); };
        const auto l_neg = [&](u32 a) { return m_builder.emit_snegate(t_long, a); };
        const auto l_lt = [&](u32 a, u32 b) {
            return m_builder.emit_integer_compare(SpvOp::OpSLessThan, t_bool, a, b);
        };
        const auto l_le = [&](u32 a, u32 b) {
            return m_builder.emit_integer_compare(SpvOp::OpSLessThanEqual, t_bool, a, b);
        };
        const auto l_eq = [&](u32 a, u32 b) {
            return m_builder.emit_integer_compare(SpvOp::OpIEqual, t_bool, a, b);
        };
        const auto l_abs = [&](u32 a) {
            return m_builder.emit_select(t_long, l_lt(a, widen(c_int(0))), l_neg(a), a);
        };
        const auto sel = [&](u32 type, u32 c, u32 a, u32 b) { return m_builder.emit_select(type, c, a, b); };
        const u32 ax = l_sub(widen(lane(m_type_int, packed, 0)), cx);
        const u32 ay = l_sub(widen(lane(m_type_int, packed, 1)), cy);
        const u32 bx = l_sub(widen(lane(m_type_int, packed, 2)), cx);
        const u32 by = l_sub(widen(lane(m_type_int, packed, 3)), cy);
        const u32 zero = widen(c_int(0));
        const u32 one = widen(c_int(1));
        const u32 bound = widen(c_int(256));
        const u32 t_true = m_builder.emit_integer_compare(SpvOp::OpIEqual, t_bool, m_const_u0, m_const_u0);
        const u32 t_false = m_builder.emit_integer_compare(SpvOp::OpINotEqual, t_bool, m_const_u0, m_const_u0);
        const u32 x_major = m_builder.emit_integer_compare(SpvOp::OpSGreaterThanEqual, t_bool,
                                                           l_abs(l_sub(bx, ax)), l_abs(l_sub(by, ay)));
        u32 ln = zero, ld = one, ls = t_false;
        u32 un = one, ud = one, us = t_false;
        u32 feasible = t_true;
        u32 b_inside = t_true;
        struct HalfPlane {
            i32 sx, sy;
            bool strict;
        };
        static constexpr HalfPlane kPlanes[4] = {{-1, -1, true}, {1, -1, true}, {-1, 1, false}, {1, 1, false}};
        for (const HalfPlane& p : kPlanes) {
            const auto h_of = [&](u32 x, u32 y) {
                const u32 hx = p.sx > 0 ? x : l_neg(x);
                const u32 hy = p.sy > 0 ? y : l_neg(y);
                return l_add(hx, hy);
            };
            const u32 ha = h_of(ax, ay);
            const u32 hb = h_of(bx, by);
            const auto within = [&](u32 v) { return p.strict ? l_lt(v, bound) : l_le(v, bound); };
            b_inside = all_of(b_inside, within(hb));
            const u32 d = l_sub(hb, ha);
            const u32 d_pos = l_lt(zero, d);
            const u32 d_neg = l_lt(d, zero);
            const u32 d_zero = m_builder.emit_logical_not(t_bool, any_of(d_pos, d_neg));
            feasible = all_of(feasible, any_of(m_builder.emit_logical_not(t_bool, d_zero), within(ha)));
            // The bound's t = cn / cd, cd > 0: an upper bound when d > 0, a
            // lower one when d < 0.
            const u32 cn = sel(t_long, d_pos, l_sub(bound, ha), l_sub(ha, bound));
            const u32 cd = sel(t_long, d_pos, d, l_neg(d));
            const u32 strict = p.strict ? t_true : t_false;
            // Upper: take a smaller bound; an equal strict one makes it strict.
            const u32 u_less = l_lt(l_mul(cn, ud), l_mul(un, cd));
            const u32 u_same = l_eq(l_mul(cn, ud), l_mul(un, cd));
            const u32 take_u = all_of(d_pos, u_less);
            un = sel(t_long, take_u, cn, un);
            ud = sel(t_long, take_u, cd, ud);
            us = sel(t_bool, take_u, strict, any_of(us, all_of(all_of(d_pos, u_same), strict)));
            // Lower: take a larger bound.
            const u32 l_more = l_lt(l_mul(ln, cd), l_mul(cn, ld));
            const u32 l_same = l_eq(l_mul(ln, cd), l_mul(cn, ld));
            const u32 take_l = all_of(d_neg, l_more);
            ln = sel(t_long, take_l, cn, ln);
            ld = sel(t_long, take_l, cd, ld);
            ls = sel(t_bool, take_l, strict, any_of(ls, all_of(all_of(d_neg, l_same), strict)));
        }
        const u32 lo_lt_hi = l_lt(l_mul(ln, ud), l_mul(un, ld));
        const u32 lo_eq_hi = l_eq(l_mul(ln, ud), l_mul(un, ld));
        const u32 meets = all_of(
            feasible,
            any_of(lo_lt_hi, all_of(lo_eq_hi, m_builder.emit_logical_not(t_bool, any_of(ls, us)))));
        // The y-major right corner (256, 0).
        const u32 ex = l_sub(bx, ax);
        const u32 ey = l_sub(by, ay);
        const u32 corner_on_line =
            all_of(l_eq(l_sub(l_mul(ex, l_neg(ay)), l_mul(ey, l_sub(bound, ax))), zero),
                   all_of(l_le(zero, l_add(l_mul(l_sub(bound, ax), ex), l_mul(l_neg(ay), ey))),
                          l_le(zero, l_add(l_mul(l_sub(bound, bx), l_neg(ex)), l_mul(l_neg(by), l_neg(ey))))));
        const u32 b_is_corner = all_of(l_eq(bx, bound), l_eq(by, zero));
        const u32 exits_x = all_of(meets, m_builder.emit_logical_not(t_bool, b_inside));
        const u32 exits_y = all_of(any_of(meets, corner_on_line),
                                   m_builder.emit_logical_not(t_bool, any_of(b_inside, b_is_corner)));
        u32 line_covered = sel(t_bool, x_major, exits_x, exits_y);
        // Depth clip at the pixel centre (the line's z/w plane).
        const u32 xc = m_builder.emit_fadd(m_type_float, fx, c_float(0.5f));
        const u32 yc = m_builder.emit_fadd(m_type_float, fy, c_float(0.5f));
        const u32 z = plane(depth, xc, yc);
        line_covered = all_of(line_covered,
                              any_of(m_builder.emit_logical_not(t_bool, depth_clip),
                                     all_of(f_le(c_float(0.0f), z), f_le(z, c_float(1.0f)))));
        const u32 full = c_uint(re.samples >= 32 ? 0xFFFFFFFFu : (1u << re.samples) - 1u);
        mask = m_builder.emit_select(m_type_uint, line_covered, full, m_const_u0);
        m_builder.emit_store(m_re_centroid_offset,
                             m_builder.emit_constant_composite(
                                 m_type_vec2, {c_float(1.0f / 16.0f), c_float(1.0f / 16.0f)}));
        covered = line_covered;
        m_builder.emit_store(m_re_inner, m_const_u0);
    } else if (tir) {
        // Every edge at the pixel centre once, then a sample's offset.
        const u32 uc = widen(m_builder.emit_iadd(m_type_int, u_base, c_int(256)));
        const u32 vc = widen(m_builder.emit_iadd(m_type_int, v_base, c_int(256)));
        std::array<u32, 4> centre{};
        for (u32 i = 0; i < edge_count; ++i) centre[i] = edge_at(edges[i], uc, vc);
        mask = m_const_u0;
        u32 centroid = m_builder.emit_constant_composite(
            m_type_vec2, {c_float(1.0f / 16.0f), c_float(1.0f / 16.0f)});
        // Lowest covered sample wins: walk the pattern backwards.
        for (u32 n = re.samples; n-- > 0;) {
            const StandardSample sample = pattern[n];
            const u32 du = widen(c_int(32 * sample.x));
            const u32 dv = widen(c_int(32 * sample.y));
            u32 inside = 0;
            for (u32 i = 0; i < edge_count; ++i) {
                const u32 value = l_add(
                    centre[i],
                    l_add(l_mul(edges[i].a, du), l_mul(edges[i].b, dv)));
                const u32 ok = l_ge0(value);
                inside = inside == 0 ? ok : all_of(inside, ok);
            }
            const u32 x = m_builder.emit_fadd(
                m_type_float, fx, c_float(0.5f + sample.x / 16.0f));
            const u32 y = m_builder.emit_fadd(
                m_type_float, fy, c_float(0.5f + sample.y / 16.0f));
            const u32 q_ok = any_of(m_builder.emit_logical_not(t_bool, clip_w),
                                    f_gt0(plane(inverse_w, x, y)));
            const u32 z = plane(depth, x, y);
            const u32 z_ok = any_of(
                m_builder.emit_logical_not(t_bool, depth_clip),
                all_of(f_le(c_float(0.0f), z), f_le(z, c_float(1.0f))));
            inside = all_of(inside, all_of(q_ok, z_ok));
            mask = m_builder.emit_bitwise_or(
                m_type_uint, mask,
                m_builder.emit_select(m_type_uint, inside, c_uint(1u << n),
                                      m_const_u0));
            const u32 at = m_builder.emit_constant_composite(
                m_type_vec2, {c_float((sample.x + 1) / 16.0f),
                              c_float((sample.y + 1) / 16.0f)});
            centroid = m_builder.emit_select(
                m_type_vec2,
                m_builder.emit_composite_construct(m_type_bvec2,
                                                   {inside, inside}),
                at, centroid);
        }
        // D3D11.3 3.5.5: the centre when every sample is covered (or none,
        // a helper pixel with the whole SampleMask), else the first covered
        // sample; the forced pattern ignores the SampleMask (3.5.6.2).
        const u32 full = c_uint(re.samples >= 32 ? 0xFFFFFFFFu
                                                 : (1u << re.samples) - 1u);
        const u32 centre_offset = m_builder.emit_constant_composite(
            m_type_vec2, {c_float(1.0f / 16.0f), c_float(1.0f / 16.0f)});
        const u32 whole = any_of(
            m_builder.emit_integer_compare(SpvOp::OpIEqual, t_bool, mask, full),
            m_builder.emit_integer_compare(SpvOp::OpIEqual, t_bool, mask,
                                           m_const_u0));
        centroid = m_builder.emit_select(
            m_type_vec2,
            m_builder.emit_composite_construct(m_type_bvec2, {whole, whole}),
            centre_offset, centroid);
        m_builder.emit_store(m_re_centroid_offset, centroid);
        covered = m_builder.emit_inotequal(t_bool, mask, m_const_u0);
        m_builder.emit_store(m_re_inner, m_const_u0);
    } else {
        // The pixel square grown by 1/512 pixel: [512 px - 1, 512 px + 513].
        const u32 x_lo = widen(m_builder.emit_isub(m_type_int, u_base, c_int(1)));
        const u32 x_hi = widen(m_builder.emit_iadd(m_type_int, u_base, c_int(513)));
        const u32 y_lo = widen(m_builder.emit_isub(m_type_int, v_base, c_int(1)));
        const u32 y_hi = widen(m_builder.emit_iadd(m_type_int, v_base, c_int(513)));
        u32 overlap = 0;
        u32 inner = m_builder.emit_logical_not(t_bool, degenerate);
        const u32 zero_long = widen(c_int(0));
        for (u32 i = 0; i < edge_count; ++i) {
            const u32 a_pos = m_builder.emit_integer_compare(
                SpvOp::OpSLessThan, t_bool, zero_long, edges[i].a);
            const u32 b_pos = m_builder.emit_integer_compare(
                SpvOp::OpSLessThan, t_bool, zero_long, edges[i].b);
            const u32 best = edge_at(
                edges[i], m_builder.emit_select(t_long, a_pos, x_hi, x_lo),
                m_builder.emit_select(t_long, b_pos, y_hi, y_lo));
            const u32 worst = edge_at(
                edges[i], m_builder.emit_select(t_long, a_pos, x_lo, x_hi),
                m_builder.emit_select(t_long, b_pos, y_lo, y_hi));
            overlap = overlap == 0 ? l_ge0(best) : all_of(overlap, l_ge0(best));
            inner = all_of(inner, l_ge0(worst));
        }
        // The bounding box's axes.
        const u32 bounds = m_builder.emit_load(m_type_ivec4, m_re_bounds);
        const auto s_le = [&](u32 a, u32 b) {  // a <= b, int
            return m_builder.emit_integer_compare(SpvOp::OpSGreaterThanEqual,
                                                  t_bool, b, a);
        };
        const u32 x_lo32 = m_builder.emit_isub(m_type_int, u_base, c_int(1));
        const u32 x_hi32 = m_builder.emit_iadd(m_type_int, u_base, c_int(513));
        const u32 y_lo32 = m_builder.emit_isub(m_type_int, v_base, c_int(1));
        const u32 y_hi32 = m_builder.emit_iadd(m_type_int, v_base, c_int(513));
        overlap = all_of(
            overlap,
            all_of(all_of(s_le(lane(m_type_int, bounds, 0), x_hi32),
                          s_le(x_lo32, lane(m_type_int, bounds, 2))),
                   all_of(s_le(lane(m_type_int, bounds, 1), y_hi32),
                          s_le(y_lo32, lane(m_type_int, bounds, 3)))));
        // 1/w > 0 and 0 <= z/w <= 1 over the grown square: the planes at
        // its corners (float; where a clip edge falls is the
        // implementation's).
        const u32 slack = c_float(1.0f / 512.0f);
        const u32 fx_lo = m_builder.emit_fsub(m_type_float, fx, slack);
        const u32 fx_hi = m_builder.emit_fadd(
            m_type_float, fx, c_float(1.0f + 1.0f / 512.0f));
        const u32 fy_lo = m_builder.emit_fsub(m_type_float, fy, slack);
        const u32 fy_hi = m_builder.emit_fadd(
            m_type_float, fy, c_float(1.0f + 1.0f / 512.0f));
        const auto plane_extreme = [&](u32 p, bool largest) {
            const u32 a_pos = f_gt0(lane(m_type_float, p, 0));
            const u32 b_pos = f_gt0(lane(m_type_float, p, 1));
            const u32 x = m_builder.emit_select(m_type_float, a_pos,
                                                largest ? fx_hi : fx_lo,
                                                largest ? fx_lo : fx_hi);
            const u32 y = m_builder.emit_select(m_type_float, b_pos,
                                                largest ? fy_hi : fy_lo,
                                                largest ? fy_lo : fy_hi);
            return plane(p, x, y);
        };
        const u32 not_clip_w = m_builder.emit_logical_not(t_bool, clip_w);
        const u32 not_depth_clip = m_builder.emit_logical_not(t_bool, depth_clip);
        const u32 q_max = plane_extreme(inverse_w, true);
        const u32 q_min = plane_extreme(inverse_w, false);
        const u32 z_max = plane_extreme(depth, true);
        const u32 z_min = plane_extreme(depth, false);
        overlap = all_of(overlap, any_of(not_clip_w, f_gt0(q_max)));
        overlap = all_of(overlap,
                         any_of(not_depth_clip,
                                all_of(f_le(c_float(0.0f), z_max),
                                       f_le(z_min, c_float(1.0f)))));
        inner = all_of(inner, any_of(not_clip_w, f_gt0(q_min)));
        inner = all_of(inner,
                       any_of(not_depth_clip,
                              all_of(f_le(c_float(0.0f), z_min),
                                     f_le(z_max, c_float(1.0f)))));
        inner = all_of(inner, overlap);
        // "the first n bits in InputCoverage from the LSB are set to 1",
        // with the SampleMask applied; InnerCoverage is not masked.
        const u32 all = re.samples >= 32 ? 0xFFFFFFFFu : (1u << re.samples) - 1u;
        mask = m_builder.emit_select(m_type_uint, overlap,
                                     c_uint(all & re.sample_mask), m_const_u0);
        m_builder.emit_store(
            m_re_inner,
            m_builder.emit_select(m_type_uint, inner, c_uint(1u), m_const_u0));
        m_builder.emit_store(
            m_re_centroid_offset,
            m_builder.emit_constant_composite(
                m_type_vec2, {c_float(1.0f / 16.0f), c_float(1.0f / 16.0f)}));
        covered = overlap;
    }
    m_builder.emit_store(m_re_mask, mask);

    // Centroid inputs of target-independent rasterization: at the forced
    // pattern's centroid.
    if (!m_re_centroid_inputs.empty()) {
        m_builder.emit_capability(SpvCapability::InterpolationFunction);
        const u32 offset = m_builder.emit_load(m_type_vec2, m_re_centroid_offset);
        for (const auto& centroid : m_re_centroid_inputs) {
            m_builder.emit_store(
                centroid.target,
                m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450,
                                        kGlslStd450InterpolateAtOffset,
                                        {centroid.source, offset}));
        }
    }

    // No coverage: a helper invocation (derivatives stay valid, side
    // effects are dropped), as for the rasterizer's own helpers.
    const u32 demote_block = m_builder.allocate_id();
    const u32 merge_block = m_builder.allocate_id();
    m_builder.emit_selection_merge(merge_block);
    m_builder.emit_branch_conditional(covered, merge_block, demote_block);
    m_builder.emit_label_id(demote_block);
    m_builder.emit_extension("SPV_EXT_demote_to_helper_invocation");
    m_builder.emit_capability(SpvCapability::DemoteToHelperInvocationEXT);
    m_builder.set_section(SpirvBuilder::Section::Functions);
    m_builder.emit(SpvOp::OpDemoteToHelperInvocationEXT);
    m_builder.emit_branch(merge_block);
    m_builder.emit_label_id(merge_block);
    return true;
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
