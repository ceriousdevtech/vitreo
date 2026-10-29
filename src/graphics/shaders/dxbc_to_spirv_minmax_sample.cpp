// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_minmax_sample.cpp
/// @brief Texture addressing and MINIMUM_/MAXIMUM_ filtering.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

u32 DxbcTranslator::widen_1d_address(u32 address4, bool arrayed,
                                      bool normalized) {
    const u32 u = m_builder.emit_composite_extract(m_type_float, address4, {0});
    const u32 w = m_builder.emit_composite_extract(m_type_float, address4, {3});
    // 0.0f and integer 0 share a bit pattern, so one constant serves both.
    const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
    const u32 row = normalized
        ? m_builder.emit_constant_float(m_type_float, 0.5f)
        : zero;
    const u32 slice = arrayed
        ? m_builder.emit_composite_extract(m_type_float, address4, {1})
        : zero;
    return m_builder.emit_composite_construct(m_type_vec4,
                                              {u, row, slice, w});
}

// MINIMUM_ / MAXIMUM_ reduction filtering (D3D11.3 5.9.4.5.6): "fetches the
// same set of texels that a normal texture filter would fetch, but instead of
// blending the values ... returns the min() or max() of the texels fetched,
// on a per-component basis"; a texel the normal filter weights 0 does not
// contribute. The set is built here from 7.18.7 (point addressing: texel
// floor(u * size)), 7.18.8 (linear: floor(u * size - 0.5) and the next, the
// next one weighted by the fraction), 7.18.10 (mip selection: POINT
// floor(LOD + 0.5), LINEAR floor(LOD) and the next, weighted by the
// fraction) and 7.18.11 (LOD from coarse derivatives / sample_d gradients,
// isotropic or anisotropic, cube faces; bias, then MinLOD/MaxLOD with MinLOD
// winning; magnification iff the clamped LOD <= 0). Precision as WARP
// (d3d11_minmax_probe [quant]): texel-weight and LOD fractions round to 8
// bits, so a neighbour counts from a fraction of 9/4096 on and a second mip
// likewise, while a POINT address takes the full-precision floor. Each texel
// is fetched at its centre and mip through s#, bound POINT-filtered with the
// state's address modes and border (7.18.9 happens in the sampler; the border
// twin completes a colour Metal lacks), and reduced with minNum / maxNum
// (NaN-aware, 3.1 "D3D arithmetic precision rules").
//
// Anisotropic (2D only, as 7.18.11's ComputeAnisotropicLOD): the spec's LOD
// and ratio of anisotropy, WARP's effective MaxAnisotropy being the power of
// two at or below the state's (the frontend passes it), then probes along the
// major axis out to WARP's measured reach (below) -- 7.18.11 "D3D 11.3 will
// allow approximations"; this is the approximation WARP makes.
u32 DxbcTranslator::emit_minmax_sample(const MinMaxSample& a) {
    auto& b = m_builder;
    b.emit_capability(SpvCapability::ImageQuery);
    const u32 tf = m_type_float;
    const u32 tb = m_type_bool;
    const auto F = [&](float v) { return b.emit_constant_float(tf, v); };
    const auto U = [&](u32 v) { return b.emit_constant_int(m_type_uint, v); };
    const auto add = [&](u32 x, u32 y) { return b.emit_fadd(tf, x, y); };
    const auto sub = [&](u32 x, u32 y) { return b.emit_fsub(tf, x, y); };
    const auto mul = [&](u32 x, u32 y) { return b.emit_fmul(tf, x, y); };
    const auto div = [&](u32 x, u32 y) { return b.emit_fdiv(tf, x, y); };
    const auto sel = [&](u32 c, u32 x, u32 y) { return b.emit_select(tf, c, x, y); };
    const auto ext = [&](u32 type, u32 op, const std::vector<u32>& args) {
        return b.emit_ext_inst(type, m_glsl_std450, op, args);
    };
    const auto floor1 = [&](u32 x) { return ext(tf, kGlslStd450Floor, {x}); };
    const auto nmin = [&](u32 x, u32 y) { return ext(tf, kGlslStd450NMin, {x, y}); };
    const auto nmax = [&](u32 x, u32 y) { return ext(tf, kGlslStd450NMax, {x, y}); };
    const auto lt = [&](u32 x, u32 y) { return b.emit_ford_lt(tb, x, y); };
    const auto ge = [&](u32 x, u32 y) { return b.emit_ford_ge(tb, x, y); };
    const auto land = [&](u32 x, u32 y) {
        return b.emit_logical(SpvOp::OpLogicalAnd, tb, x, y);
    };
    const auto lor = [&](u32 x, u32 y) {
        return b.emit_logical(SpvOp::OpLogicalOr, tb, x, y);
    };
    const auto lnot = [&](u32 x) { return b.emit_logical_not(tb, x); };
    const auto comp = [&](u32 v, u32 i) {
        return b.emit_composite_extract(tf, v, {i});
    };
    const u32 true_ = b.emit_integer_compare(SpvOp::OpIEqual, tb, m_const_u0,
                                             m_const_u0);

    // The sampler state (push member 13) and its MipLODBias (member 0).
    const auto state_word = [&](u32 component) {
        return b.emit_load(
            m_type_uint,
            b.emit_access_chain(m_type_ptr_push_uint, push_block_var(),
                                {U(kPushMemberMinMaxSamplers), U(a.slot),
                                 U(component)}));
    };
    const u32 filter = state_word(0);
    const u32 min_lod = b.emit_bitcast(tf, state_word(1));
    const u32 max_lod = b.emit_bitcast(tf, state_word(2));
    const u32 max_aniso = b.emit_convert_u_to_f(tf, state_word(3));
    const u32 bias = b.emit_load(
        tf, b.emit_access_chain(m_type_ptr_push_float, push_block_var(),
                                {U(0), U(a.slot)}));
    const auto has = [&](u32 mask) {
        return b.emit_inotequal(tb, b.emit_bitwise_and(m_type_uint, filter,
                                                       U(mask)),
                                m_const_u0);
    };
    // D3D11_FILTER: 0x100 MINIMUM, 0x180 MAXIMUM; low bits 0x55 ANISOTROPIC,
    // else MIN (0x10) / MAG (0x04) / MIP (0x01) LINEAR.
    const u32 is_max = has(0x80u);
    const u32 anisotropic = b.emit_integer_compare(
        SpvOp::OpIEqual, tb, b.emit_bitwise_and(m_type_uint, filter, U(0x7Fu)),
        U(0x55u));
    const u32 min_linear = lor(anisotropic, has(0x10u));
    const u32 mag_linear = lor(anisotropic, has(0x04u));
    const u32 mip_linear = lor(anisotropic, has(0x01u));

    // The view's mips and sizes.
    const bool filter_v = !a.texture_1d;
    const u32 size_type =
        (a.volume || a.arrayed) ? m_type_ivec3 : m_type_ivec2;
    const auto level_size = [&](u32 level) {
        const u32 q = b.emit_image_query_size(size_type, a.image, level);
        std::array<u32, 3> r{};
        for (u32 i = 0; i < 2; ++i) {
            r[i] = b.emit_convert_s_to_f(
                tf, b.emit_composite_extract(m_type_int, q, {i}));
        }
        r[2] = a.volume ? b.emit_convert_s_to_f(
                              tf, b.emit_composite_extract(m_type_int, q, {2}))
                        : F(1.0f);
        return r;
    };
    const u32 levels = b.emit_convert_s_to_f(
        tf, b.emit_image_query_levels(m_type_int, a.image));
    const auto size0 = level_size(b.emit_constant_int(m_type_int, 0u));

    // Normalized coordinates (face-local for a cube: 7.18.7), and the layer.
    u32 cu = 0, cv = 0, cw = F(0.0f), layer = 0;
    u32 ma_vec = 0, s_vec = 0, t_vec = 0, axis_major = 0, sc = 0, tc = 0;
    u32 direction = 0;
    if (a.cube) {
        const u32 x = comp(a.coordinate, 0);
        const u32 y = comp(a.coordinate, 1);
        const u32 z = comp(a.coordinate, 2);
        if (a.arrayed) layer = comp(a.coordinate, 3);
        direction = b.emit_composite_construct(m_type_vec3, {x, y, z});
        const auto fabs1 = [&](u32 v) { return ext(tf, kGlslStd450FAbs, {v}); };
        const u32 ax = fabs1(x), ay = fabs1(y), az = fabs1(z);
        // "In the case of a tie, the following precedence should occur: Z,
        // Y, X."
        const u32 z_major = land(ge(az, ax), ge(az, ay));
        const u32 y_major = land(lnot(z_major), ge(ay, ax));
        const u32 positive =
            ge(sel(z_major, z, sel(y_major, y, x)), F(0.0f));
        const auto v3 = [&](float i, float j, float k) {
            return b.emit_constant_composite(m_type_vec3, {F(i), F(j), F(k)});
        };
        // Faces +X -X +Y -Y +Z -Z (5.3.8: the minor axes selected and
        // mirrored as u = sc / |ma|, v = tc / |ma|).
        const auto pick = [&](const std::array<u32, 6>& face) {
            const u32 xf = b.emit_select(m_type_vec3, positive, face[0], face[1]);
            const u32 yf = b.emit_select(m_type_vec3, positive, face[2], face[3]);
            const u32 zf = b.emit_select(m_type_vec3, positive, face[4], face[5]);
            return b.emit_select(m_type_vec3, z_major, zf,
                                 b.emit_select(m_type_vec3, y_major, yf, xf));
        };
        ma_vec = pick({v3(1, 0, 0), v3(-1, 0, 0), v3(0, 1, 0), v3(0, -1, 0),
                       v3(0, 0, 1), v3(0, 0, -1)});
        s_vec = pick({v3(0, 0, -1), v3(0, 0, 1), v3(1, 0, 0), v3(1, 0, 0),
                      v3(1, 0, 0), v3(-1, 0, 0)});
        t_vec = pick({v3(0, -1, 0), v3(0, -1, 0), v3(0, 0, 1), v3(0, 0, -1),
                      v3(0, -1, 0), v3(0, -1, 0)});
        axis_major = b.emit_dot(tf, direction, ma_vec);
        sc = b.emit_dot(tf, direction, s_vec);
        tc = b.emit_dot(tf, direction, t_vec);
        cu = add(mul(div(sc, axis_major), F(0.5f)), F(0.5f));
        cv = add(mul(div(tc, axis_major), F(0.5f)), F(0.5f));
    } else {
        cu = comp(a.coordinate, 0);
        cv = comp(a.coordinate, 1);  // a Texture1D's widened row (0.5)
        if (a.volume) cw = comp(a.coordinate, 2);
        if (a.arrayed) layer = comp(a.coordinate, 2);
    }

    // The LOD (7.18.11).
    u32 lod = 0;
    u32 aniso_ratio = F(1.0f);
    u32 major_u = F(0.0f), major_v = F(0.0f);
    // ComputeAnisotropicLOD is Texture2D (and 2D arrays) only; WARP filters
    // a Texture1D isotropically too (d3d11_minmax_probe [tex] "T1 ...
    // MAX_ANISO ... grad": the isotropic LOD's mips).
    const bool may_be_anisotropic = a.lod != MinMaxSample::Lod::Explicit &&
                                    !a.cube && !a.volume && !a.texture_1d;
    if (a.lod == MinMaxSample::Lod::Explicit) {
        lod = add(a.lod_operand, bias);
    } else {
        std::array<u32, 3> dx{}, dy{};
        if (a.lod == MinMaxSample::Lod::Implicit) {
            // deriv_rtx_coarse / deriv_rty_coarse (22.5.2/3): the quad's first
            // lane's fine derivative, broadcast to the quad.
            if (!m_derivative_control_capability_emitted) {
                b.emit_capability(SpvCapability::DerivativeControl);
                m_derivative_control_capability_emitted = true;
            }
            b.emit_capability(SpvCapability::GroupNonUniform);
            b.emit_capability(SpvCapability::GroupNonUniformQuad);
            const u32 source = a.cube
                ? b.emit_composite_construct(
                      m_type_vec4, {comp(direction, 0), comp(direction, 1),
                                    comp(direction, 2), F(0.0f)})
                : b.emit_composite_construct(m_type_vec4,
                                             {cu, cv, cw, F(0.0f)});
            const auto coarse = [&](SpvOp op) {
                return b.emit_quad_broadcast(
                    m_type_vec4, U(3u),
                    b.emit_derivative(op, m_type_vec4, source), m_const_u0);
            };
            const u32 ddx = coarse(SpvOp::OpDPdxFine);
            const u32 ddy = coarse(SpvOp::OpDPdyFine);
            for (u32 i = 0; i < 3; ++i) {
                dx[i] = comp(ddx, i);
                dy[i] = comp(ddy, i);
            }
        } else {
            for (u32 i = 0; i < 3; ++i) {
                dx[i] = comp(a.gradient_x, i);
                dy[i] = comp(a.gradient_y, i);
            }
        }
        if (a.texture_1d) {
            dx[1] = dx[2] = dy[1] = dy[2] = F(0.0f);
        } else if (!a.volume && !a.cube) {
            dx[2] = dy[2] = F(0.0f);
        }
        // Texel-space derivatives: the size of the most detailed mip; on a
        // cube, the face's (AxisMajor * d' - TC' * dMajor) / AxisMajor^2 at
        // half the face size.
        std::array<u32, 3> tx{}, ty{};
        if (a.cube) {
            const u32 half = mul(F(0.5f), size0[0]);
            const u32 am2 = mul(axis_major, axis_major);
            const auto project = [&](const std::array<u32, 3>& d,
                                     std::array<u32, 3>& out) {
                const u32 dv = b.emit_composite_construct(m_type_vec3,
                                                          {d[0], d[1], d[2]});
                const u32 ds = b.emit_dot(tf, dv, s_vec);
                const u32 dt = b.emit_dot(tf, dv, t_vec);
                const u32 dm = b.emit_dot(tf, dv, ma_vec);
                out[0] = mul(div(sub(mul(axis_major, ds), mul(sc, dm)), am2),
                             half);
                out[1] = mul(div(sub(mul(axis_major, dt), mul(tc, dm)), am2),
                             half);
                out[2] = F(0.0f);
            };
            project(dx, tx);
            project(dy, ty);
        } else {
            for (u32 i = 0; i < 3; ++i) {
                tx[i] = mul(dx[i], size0[i]);
                ty[i] = mul(dy[i], size0[i]);
            }
        }
        const auto length = [&](const std::array<u32, 3>& v) {
            return ext(tf, kGlslStd450Sqrt,
                       {add(add(mul(v[0], v[0]), mul(v[1], v[1])),
                            mul(v[2], v[2]))});
        };
        const u32 isotropic =
            ext(tf, kGlslStd450Log2, {nmax(length(tx), length(ty))});
        lod = isotropic;
        if (may_be_anisotropic) {
            const u32 sq_x = add(mul(tx[0], tx[0]), mul(tx[1], tx[1]));
            const u32 sq_y = add(mul(ty[0], ty[0]), mul(ty[1], ty[1]));
            const u32 determinant = ext(
                tf, kGlslStd450FAbs,
                {sub(mul(tx[0], ty[1]), mul(tx[1], ty[0]))});
            const u32 major_x = lt(sq_y, sq_x);
            const u32 sq_major = sel(major_x, sq_x, sq_y);
            const u32 length_major = ext(tf, kGlslStd450Sqrt, {sq_major});
            const u32 ratio = div(sq_major, determinant);
            const u32 clamped_ratio = lt(max_aniso, ratio);
            u32 ratio1 = sel(clamped_ratio, max_aniso, ratio);
            const u32 length_minor =
                sel(clamped_ratio, div(length_major, max_aniso),
                    div(determinant, length_major));
            ratio1 = sel(lt(length_minor, F(1.0f)),
                         nmax(F(1.0f), mul(ratio1, length_minor)), ratio1);
            // A zero-area or NaN footprint: the isotropic LOD.
            const u32 valid = ge(ratio, F(1.0f));
            const u32 anisotropic_lod =
                sel(valid, ext(tf, kGlslStd450Log2, {length_minor}), isotropic);
            lod = sel(anisotropic, anisotropic_lod, isotropic);
            aniso_ratio = sel(land(anisotropic, valid), ratio1, F(1.0f));
            major_u = sel(major_x, dx[0], dy[0]);
            major_v = sel(major_x, dx[1], dy[1]);
        }
        lod = add(lod, bias);
        if (a.lod_operand != 0) lod = add(lod, a.lod_operand);  // sample_b
    }
    // "clampedLOD = max(MinLOD, min(MaxLOD, biasedLOD))"; magnification iff
    // clampedLOD <= 0.
    const u32 clamped = nmax(min_lod, nmin(max_lod, lod));
    const u32 magnify = ge(F(0.0f), clamped);
    const u32 linear = b.emit_select(tb, magnify, mag_linear, min_linear);
    // Mip selection on the LOD in 8.8 fixed point.
    const u32 bounded = nmax(F(-1.0f), nmin(clamped, levels));
    const u32 quantized = div(
        ext(tf, kGlslStd450RoundEven, {mul(bounded, F(256.0f))}), F(256.0f));
    const u32 floor_mip = floor1(quantized);
    const u32 has_second_mip = land(mip_linear, lt(floor_mip, quantized));
    const u32 nearest_mip = floor1(add(quantized, F(0.5f)));
    const u32 first_mip = sel(mip_linear, floor_mip, nearest_mip);
    const u32 top = sub(levels, F(1.0f));
    const auto clamp_level = [&](u32 m) { return nmax(F(0.0f), nmin(m, top)); };
    const u32 level0 = clamp_level(first_mip);
    const u32 level1 = clamp_level(add(first_mip, F(1.0f)));
    const u32 second_mip = land(has_second_mip, lt(level0, level1));
    const auto level_int = [&](u32 level) {
        return b.emit_convert_f_to_s(m_type_int, level);
    };
    const auto size_l0 = level_size(level_int(level0));
    const auto size_l1 = level_size(level_int(level1));

    // Probes along the anisotropy line (one, at the coordinate, otherwise),
    // placed as WARP places them (d3d11_minmax_probe [reach] sweeps the
    // coordinate across a texel in 1/256 steps; [aniso] checks the extents):
    // for a ratio r <= 2, two probes (r - 1) / 2 texels either side of the
    // coordinate; above, N = the effective MaxAnisotropy probes, r / (N + 1)
    // apart and centred on it, so the outermost sit r (N - 1) / (2 (N + 1))
    // away. Distances are in texels of the mip sampled, along the major axis.
    // A probe's position is floored to 1/256 texel (not rounded like the
    // isotropic coordinate): the reach sweeps put every MINIMUM extent a
    // step further out than the MAXIMUM one.
    u32 probe_count = U(1u);
    u32 reach = F(0.0f), step = F(0.0f);
    u32 dir_u = F(0.0f), dir_v = F(0.0f);
    u32 probing = 0;  // bool id when probes may be anisotropic
    if (may_be_anisotropic) {
        const u32 active = lt(F(1.0f), aniso_ratio);
        const u32 pair = ge(F(2.0f), aniso_ratio);
        const u32 n = nmax(F(1.0f), max_aniso);
        reach = sel(active,
                    sel(pair, mul(sub(aniso_ratio, F(1.0f)), F(0.5f)),
                        div(mul(aniso_ratio, sub(n, F(1.0f))),
                            mul(F(2.0f), add(n, F(1.0f))))),
                    F(0.0f));
        const u32 count = sel(active, sel(pair, F(2.0f), nmax(F(2.0f), n)),
                              F(1.0f));
        step = sel(active, div(mul(F(2.0f), reach),
                               nmax(F(1.0f), sub(count, F(1.0f)))),
                   F(0.0f));
        probe_count = b.emit_convert_f_to_u(m_type_uint, count);
        probing = active;
        // The major axis as a unit vector in texel space.
        const u32 texel_u = mul(major_u, size0[0]);
        const u32 texel_v = mul(major_v, size0[1]);
        const u32 inv = div(F(1.0f), ext(tf, kGlslStd450Sqrt,
                                         {add(mul(texel_u, texel_u),
                                              mul(texel_v, texel_v))}));
        dir_u = sel(active, mul(texel_u, inv), F(0.0f));
        dir_v = sel(active, mul(texel_v, inv), F(0.0f));
    }

    // One mip's footprint at a probe, folded into `acc`.
    // `ou` / `ov`: the probe's offset from the coordinate, in texels.
    const auto fold_level = [&](u32 level, const std::array<u32, 3>& size,
                                u32 pu, u32 pv, u32 pw, u32 ou, u32 ov,
                                u32 level_on, u32 acc) {
        struct Axis {
            u32 first{0};
            u32 second{0};  // bool: the next texel is weighted
        };
        const auto axis = [&](u32 coordinate, u32 extent, i32 offset,
                              u32 probe_offset) {
            const u32 scaled = mul(coordinate, extent);
            u32 texel_space = sub(scaled, F(0.5f));
            if (probe_offset != 0) texel_space = add(texel_space, probe_offset);
            const u32 left = floor1(texel_space);
            const u32 fixed = mul(sub(texel_space, left), F(256.0f));
            const u32 nearest = ext(tf, kGlslStd450RoundEven, {fixed});
            const u32 fraction = div(
                probing != 0 ? sel(probing, floor1(fixed), nearest) : nearest,
                F(256.0f));
            const u32 carry = ge(fraction, F(1.0f));
            Axis out;
            out.first = sel(linear, sel(carry, add(left, F(1.0f)), left),
                            floor1(scaled));
            out.second =
                land(linear, land(lnot(carry), lt(F(0.0f), fraction)));
            if (offset != 0) {
                out.first = add(out.first, F(static_cast<float>(offset)));
            }
            return out;
        };
        const Axis au = axis(pu, size[0], a.offset[0], ou);
        const Axis av =
            filter_v ? axis(pv, size[1], a.offset[1], ov) : Axis{};
        const Axis aw = a.volume ? axis(pw, size[2], a.offset[2], 0) : Axis{};
        const auto centre = [&](u32 index, u32 extent) {
            return div(add(index, F(0.5f)), extent);
        };
        for (u32 du = 0; du < 2; ++du) {
            for (u32 dv = 0; dv < (filter_v ? 2u : 1u); ++dv) {
                for (u32 dw = 0; dw < (a.volume ? 2u : 1u); ++dw) {
                    u32 include = level_on;
                    if (du != 0) include = land(include, au.second);
                    if (dv != 0) include = land(include, av.second);
                    if (dw != 0) include = land(include, aw.second);
                    const u32 tu = centre(
                        du != 0 ? add(au.first, F(1.0f)) : au.first, size[0]);
                    const u32 tv = filter_v
                        ? centre(dv != 0 ? add(av.first, F(1.0f)) : av.first,
                                 size[1])
                        : cv;
                    u32 where = 0;
                    if (a.cube) {
                        const u32 s_ndc = sub(mul(tu, F(2.0f)), F(1.0f));
                        const u32 t_ndc = sub(mul(tv, F(2.0f)), F(1.0f));
                        const auto scaled = [&](u32 vec, u32 scalar) {
                            return b.emit_fmul(
                                m_type_vec3, vec,
                                b.emit_composite_construct(
                                    m_type_vec3, {scalar, scalar, scalar}));
                        };
                        const u32 dir = b.emit_fadd(
                            m_type_vec3, ma_vec,
                            b.emit_fadd(m_type_vec3, scaled(s_vec, s_ndc),
                                        scaled(t_vec, t_ndc)));
                        where = a.arrayed
                            ? b.emit_composite_construct(
                                  m_type_vec4, {comp(dir, 0), comp(dir, 1),
                                                comp(dir, 2), layer})
                            : dir;
                    } else if (a.volume) {
                        const u32 tw = centre(
                            dw != 0 ? add(aw.first, F(1.0f)) : aw.first,
                            size[2]);
                        where = b.emit_composite_construct(m_type_vec3,
                                                           {tu, tv, tw});
                    } else if (a.arrayed) {
                        where = b.emit_composite_construct(m_type_vec3,
                                                           {tu, tv, layer});
                    } else {
                        where = b.emit_composite_construct(m_type_vec2,
                                                           {tu, tv});
                    }
                    u32 texel = b.emit_image_sample_explicit_lod(
                        m_type_vec4, a.sampled_image, where, level);
                    if (a.twin_sampled_image != 0) {
                        const u32 hi = b.emit_image_sample_explicit_lod(
                            m_type_vec4, a.twin_sampled_image, where, level);
                        texel = b.emit_fadd(
                            m_type_vec4, texel,
                            b.emit_fmul(m_type_vec4,
                                        b.emit_fsub(m_type_vec4, hi, texel),
                                        a.border_colour));
                    }
                    const u32 reduced = b.emit_select(
                        m_type_vec4, is_max,
                        ext(m_type_vec4, kGlslStd450NMax, {acc, texel}),
                        ext(m_type_vec4, kGlslStd450NMin, {acc, texel}));
                    acc = b.emit_select(m_type_vec4, include, reduced, acc);
                }
            }
        }
        return acc;
    };

    // for (k = 0; k < probe_count; ++k) acc = fold(probe k); acc starts as NaN,
    // which minNum / maxNum drop at the first texel.
    const u32 nan = b.emit_bitcast(m_type_vec4,
                                   make_uvec4_imm(0x7FC00000u, 0x7FC00000u,
                                                  0x7FC00000u, 0x7FC00000u));
    const u32 one_u = U(1u);
    const u32 pre = b.allocate_id();
    const u32 header = b.allocate_id();
    const u32 body = b.allocate_id();
    const u32 continue_block = b.allocate_id();
    const u32 merge = b.allocate_id();
    const u32 acc_next = b.allocate_id();
    const u32 k_next = b.allocate_id();
    b.emit_branch(pre);
    b.emit_label_id(pre);
    b.emit_branch(header);
    b.emit_label_id(header);
    const u32 acc = b.emit_phi(m_type_vec4, {{nan, pre}, {acc_next, continue_block}});
    const u32 k = b.emit_phi(m_type_uint, {{m_const_u0, pre}, {k_next, continue_block}});
    b.emit_loop_merge(merge, continue_block);
    b.emit_branch_conditional(
        b.emit_integer_compare(SpvOp::OpULessThan, tb, k, probe_count), body,
        merge);
    b.emit_label_id(body);
    // This probe's offset, in texels of the mip sampled, along the axis.
    u32 ou = 0, ov = 0;
    if (may_be_anisotropic) {
        const u32 t = sub(mul(b.emit_convert_u_to_f(tf, k), step), reach);
        ou = mul(t, dir_u);
        ov = mul(t, dir_v);
    }
    u32 folded =
        fold_level(level0, size_l0, cu, cv, cw, ou, ov, true_, acc);
    folded = fold_level(level1, size_l1, cu, cv, cw, ou, ov, second_mip,
                        folded);
    b.emit_branch(continue_block);
    b.emit_label_id(continue_block);
    b.set_section(SpirvBuilder::Section::Functions);
    b.emit(SpvOp::OpVectorShuffle,
           {m_type_vec4, acc_next, folded, folded, 0u, 1u, 2u, 3u});
    b.emit(SpvOp::OpIAdd, {m_type_uint, k_next, k, one_u});
    b.emit_branch(header);
    b.emit_label_id(merge);
    return acc;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
