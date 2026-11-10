// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_vitreo_tex.cpp
/// @brief VX_TEX (dxbc_vitreo_ext.hpp): texture operations outside the
/// pixel stage and comparison sampling with gradients.
///
/// SampleCmpGrad is sample_c with sample_d's gradients and runs through
/// translate_sample_operation. LodGrad is D3D11.3 22.5.6 lod computed from
/// explicit gradients by the 7.18.11 rules, for a Metal kernel, which has
/// no implicit derivatives and so no calculate_*_lod of its own:
///  - TextureCube: the derivatives move into the major face's space
///    (precedence Z, Y, X on ties) and scale by half the face size;
///    everything else scales by the most detailed view mip's texel counts;
///  - ANISOTROPIC (2D and 2D arrays only): the ellipse's orthogonal axes
///    ([Heckbert 89], the 7.18.11 transformation, skipped for a zero,
///    parallel, perpendicular or non-finite pair or one that yields a NaN),
///    then the ratio of anisotropy clamped to the sampler's MaxAnisotropy
///    and LOD = log2(minor axis); otherwise LOD = log2(max(|dX|, |dY|));
///  - MipLODBias then MinLOD / MaxLOD (max(MinLOD, min(MaxLOD, LOD))) and
///    the view's mip range give ClampedLOD; NonClampedLOD is the biased
///    LOD with no clamp, as the pixel-stage lod in this translator returns.
/// The sampler state comes from DxbcTranslationOptions::sampler_state_buffer
/// (filter, MaxAnisotropy, MinLOD, MaxLOD, MipLODBias).

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"

#include <limits>

namespace vitreo::graphics::dxbc_spirv_detail {

using namespace dxbc_vitreo_ext;

bool DxbcTranslator::translate_vx_tex(u32 control) {
    const TexOp op = static_cast<TexOp>(control & 0xFFu);
    if (op == TexOp::SampleCmpGrad) {
        m_tiled = TiledInstruction{};
        m_tiled.active = true;
        m_tiled.clamp = (control & kTexClamp) != 0;
        m_vx_compare_gradient = true;
        const bool ok = translate_sample_operation(0, Sm4Opcode::SAMPLE_C, control & kTexOffsetMask);
        m_vx_compare_gradient = false;
        m_tiled = TiledInstruction{};
        return ok;
    }
    if (op == TexOp::SampleCmpLevel || op == TexOp::SampleCmpBias) {
        m_tiled = TiledInstruction{};
        m_tiled.active = true;
        m_tiled.clamp = (control & kTexClamp) != 0;
        bool& flag = op == TexOp::SampleCmpLevel ? m_vx_compare_level : m_vx_compare_bias;
        flag = true;
        const bool ok = translate_sample_operation(
            0, op == TexOp::SampleCmpLevel ? Sm4Opcode::SAMPLE_C_LZ : Sm4Opcode::SAMPLE_C,
            control & kTexOffsetMask);
        flag = false;
        m_tiled = TiledInstruction{};
        return ok;
    }
    if (op != TexOp::LodGrad) return fail("VX_TEX: unknown operation " + std::to_string(control & 0xFFu));

    Sm4Operand dst, address, resource, sampler, dx_operand, dy_operand;
    if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(address) ||
        !m_decoder.decode_operand(resource) || !m_decoder.decode_operand(sampler) ||
        !m_decoder.decode_operand(dx_operand) || !m_decoder.decode_operand(dy_operand)) {
        return fail("VX_TEX LodGrad: bad operands");
    }
    if (resource.type != Sm4OperandType::Resource || sampler.type != Sm4OperandType::Sampler) {
        return fail("VX_TEX LodGrad: expected t# and s# operands");
    }
    const auto texture_it = m_texture_vars.find(resource.index0);
    if (texture_it == m_texture_vars.end() || m_sampler_vars.find(sampler.index0) == m_sampler_vars.end() ||
        m_texture_multisampled.contains(resource.index0)) {
        return fail("VX_TEX LodGrad: undeclared or multisampled resource");
    }
    if (m_sampler_state_var == 0) {
        return fail("VX_TEX LodGrad: the sampler's filter and LOD clamps need "
                    "DxbcTranslationOptions::sampler_state_buffer");
    }
    const SpvDim dimension = m_texture_dimensions.at(resource.index0);
    const bool arrayed = m_texture_arrays.contains(resource.index0);
    const bool cube = dimension == SpvDim::DimCube;
    const bool volume = dimension == SpvDim::Dim3D;
    const bool one_d = m_texture_1d.contains(resource.index0);
    const u32 return_type = m_texture_return_types.at(resource.index0);
    if (return_type == 3u || return_type == 4u) return fail("VX_TEX LodGrad: integer textures are not filtered");
    const u32 image_type = arrayed && cube ? m_type_image_cube_array
        : arrayed ? m_type_image_2d_array
        : cube ? m_type_image_cube
        : volume ? m_type_image_3d : m_type_image_2d;
    m_builder.emit_capability(SpvCapability::ImageQuery);
    const u32 image = m_builder.emit_load(image_type, texture_it->second);

    auto& b = m_builder;
    const u32 f = m_type_float;
    const auto k = [&](float v) { return b.emit_constant_float(f, v); };
    const auto add = [&](u32 x, u32 y) { return b.emit_fadd(f, x, y); };
    const auto sub = [&](u32 x, u32 y) { return b.emit_fsub(f, x, y); };
    const auto mul = [&](u32 x, u32 y) { return b.emit_fmul(f, x, y); };
    const auto div = [&](u32 x, u32 y) { return b.emit_fdiv(f, x, y); };
    const auto ext1 = [&](u32 inst, u32 x) { return b.emit_ext_inst(f, m_glsl_std450, inst, {x}); };
    const auto ext2 = [&](u32 inst, u32 x, u32 y) { return b.emit_ext_inst(f, m_glsl_std450, inst, {x, y}); };
    const auto fabs = [&](u32 x) { return ext1(kGlslStd450FAbs, x); };
    const auto fsqrt = [&](u32 x) { return ext1(kGlslStd450Sqrt, x); };
    const auto fmax = [&](u32 x, u32 y) { return ext2(kGlslStd450FMax, x, y); };
    const auto fmin = [&](u32 x, u32 y) { return ext2(kGlslStd450FMin, x, y); };
    const auto sel = [&](u32 c, u32 x, u32 y) { return b.emit_select(f, c, x, y); };
    const auto ge = [&](u32 x, u32 y) { return b.emit_ford_ge(m_type_bool, x, y); };
    const auto lt = [&](u32 x, u32 y) { return b.emit_ford_lt(m_type_bool, x, y); };
    const auto land = [&](u32 x, u32 y) { return b.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, x, y); };
    const auto lor = [&](u32 x, u32 y) { return b.emit_logical(SpvOp::OpLogicalOr, m_type_bool, x, y); };
    const auto lnot = [&](u32 x) { return b.emit_logical_not(m_type_bool, x); };
    const auto comp = [&](u32 v4, u32 i) { return b.emit_composite_extract(f, v4, {i}); };
    const auto sign = [&](u32 x) { return sel(lt(x, k(0.0f)), k(-1.0f), k(1.0f)); };
    // finite: |x| < inf (false for NaN and infinities).
    const u32 inf = b.emit_constant_float(f, std::numeric_limits<float>::infinity());
    const auto finite = [&](u32 x) { return lt(fabs(x), inf); };

    u32 address4 = 0, dx4 = 0, dy4 = 0;
    if (!load_vec4(address, address4) || !load_vec4(dx_operand, dx4) || !load_vec4(dy_operand, dy4)) return false;

    // The most detailed view mip's size, as floats.
    const u32 size_type = arrayed && cube ? m_type_ivec3
        : arrayed ? m_type_ivec3
        : cube ? m_type_ivec2
        : volume ? m_type_ivec3 : m_type_ivec2;
    const u32 size = b.emit_image_query_size(size_type, image, b.emit_constant_int(m_type_int, 0u));
    const auto size_f = [&](u32 i) {
        return b.emit_convert_s_to_f(f, b.emit_composite_extract(m_type_int, size, {i}));
    };
    u32 xu = comp(dx4, 0), xv = comp(dx4, 1), xw = comp(dx4, 2);
    u32 yu = comp(dy4, 0), yv = comp(dy4, 1), yw = comp(dy4, 2);
    if (cube) {
        // 7.18.11: TC'.uv / AxisMajor on the major face; its derivative is
        // (AxisMajor * d' - TC' * dMajor) / AxisMajor^2, where AxisMajor =
        // |major| moves with sign(major) * dMajor.
        const u32 tx = comp(address4, 0), ty = comp(address4, 1), tz = comp(address4, 2);
        const u32 ax = fabs(tx), ay = fabs(ty), az = fabs(tz);
        const u32 z_major = land(ge(az, ay), ge(az, ax));
        const u32 y_major = land(lnot(z_major), ge(ay, ax));
        // Face coordinates (D3D TextureCube): +X (-z, -y), -X (z, -y),
        // +Y (x, z), -Y (x, -z), +Z (x, -y), -Z (-x, -y). The same linear map
        // applies to the derivatives.
        const auto face_uv = [&](u32 x, u32 y, u32 z, u32& u, u32& v) {
            const u32 x_u = mul(b.emit_fnegate(f, sign(tx)), z);
            const u32 y_v = mul(sign(ty), z);
            const u32 z_u = mul(sign(tz), x);
            u = sel(z_major, z_u, sel(y_major, x, x_u));
            v = sel(z_major, b.emit_fnegate(f, y), sel(y_major, y_v, b.emit_fnegate(f, y)));
        };
        u32 tu = 0, tv = 0, du = 0, dv = 0, eu = 0, ev = 0;
        face_uv(tx, ty, tz, tu, tv);
        face_uv(xu, xv, xw, du, dv);
        face_uv(yu, yv, yw, eu, ev);
        const u32 major = sel(z_major, az, sel(y_major, ay, ax));
        const u32 major_sign = sel(z_major, sign(tz), sel(y_major, sign(ty), sign(tx)));
        const u32 dmx = mul(major_sign, sel(z_major, xw, sel(y_major, xv, xu)));
        const u32 dmy = mul(major_sign, sel(z_major, yw, sel(y_major, yv, yu)));
        const u32 m2 = mul(major, major);
        const u32 half_side = mul(k(0.5f), size_f(0));
        xu = mul(div(sub(mul(major, du), mul(tu, dmx)), m2), half_side);
        xv = mul(div(sub(mul(major, dv), mul(tv, dmx)), m2), half_side);
        yu = mul(div(sub(mul(major, eu), mul(tu, dmy)), m2), half_side);
        yv = mul(div(sub(mul(major, ev), mul(tv, dmy)), m2), half_side);
        xw = yw = k(0.0f);
    } else {
        const u32 su = size_f(0);
        xu = mul(xu, su);
        yu = mul(yu, su);
        if (one_d) {
            xv = yv = xw = yw = k(0.0f);
        } else {
            const u32 sv = size_f(1);
            xv = mul(xv, sv);
            yv = mul(yv, sv);
            if (volume) {
                const u32 sw = size_f(2);
                xw = mul(xw, sw);
                yw = mul(yw, sw);
            } else {
                xw = yw = k(0.0f);
            }
        }
    }

    const u32 flags = sampler_state_word(sampler, kSamplerStateFlagsWord);
    const u32 filter = b.emit_bitwise_and(
        m_type_uint, b.emit_shift_right_logical(m_type_uint, flags, b.emit_constant_int(m_type_uint, kSamplerStateFilterShift)),
        b.emit_constant_int(m_type_uint, 0x7Fu));
    // ANISOTROPIC and COMPARISON_ / MINIMUM_ / MAXIMUM_ANISOTROPIC: the low
    // seven filter bits 0x55 (the backend sampler's own test).
    const u32 anisotropic_filter =
        b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, filter, b.emit_constant_int(m_type_uint, 0x55u));
    const u32 max_aniso = b.emit_convert_u_to_f(
        f, b.emit_bitwise_and(m_type_uint,
                              b.emit_shift_right_logical(m_type_uint, flags,
                                                         b.emit_constant_int(m_type_uint, kSamplerStateFlagsMaxAnisoShift)),
                              b.emit_constant_int(m_type_uint, kSamplerStateAnisoMask)));

    const u32 length_x = fsqrt(add(add(mul(xu, xu), mul(xv, xv)), mul(xw, xw)));
    const u32 length_y = fsqrt(add(add(mul(yu, yu), mul(yv, yv)), mul(yw, yw)));
    u32 lod = ext1(kGlslStd450Log2, fmax(length_x, length_y));
    if (!cube && !volume && !one_d) {
        // The orthogonal Jacobian ([Heckbert 89], 7.18.11).
        const u32 A = add(mul(xv, xv), mul(yv, yv));
        const u32 B = mul(k(-2.0f), add(mul(xu, xv), mul(yu, yv)));
        const u32 C = add(mul(xu, xu), mul(yu, yu));
        const u32 det0 = sub(mul(xu, yv), mul(yu, xv));
        const u32 F = mul(det0, det0);
        const u32 p = sub(A, C), q = add(A, C);
        const u32 t = fsqrt(add(mul(p, p), mul(B, B)));
        const u32 sb = sign(B);
        const u32 nxu = fsqrt(div(mul(F, add(t, p)), mul(t, add(q, t))));
        const u32 nxv = mul(fsqrt(div(mul(F, sub(t, p)), mul(t, add(q, t)))), sb);
        const u32 nyu = mul(fsqrt(div(mul(F, sub(t, p)), mul(t, sub(q, t)))), b.emit_fnegate(f, sb));
        const u32 nyv = fsqrt(div(mul(F, add(t, p)), mul(t, sub(q, t))));
        const u32 zero = k(0.0f);
        const u32 inputs_finite = land(land(finite(xu), finite(xv)), land(finite(yu), finite(yv)));
        const u32 nonzero = land(lt(zero, length_x), lt(zero, length_y));
        const u32 not_parallel = lt(zero, F);
        const u32 not_perpendicular = lt(zero, fabs(B));
        const u32 outputs_finite = land(land(finite(nxu), finite(nxv)), land(finite(nyu), finite(nyv)));
        const u32 transform =
            land(land(inputs_finite, nonzero), land(land(not_parallel, not_perpendicular), outputs_finite));
        const u32 au = sel(transform, nxu, xu), av = sel(transform, nxv, xv);
        const u32 bu = sel(transform, nyu, yu), bv = sel(transform, nyv, yv);
        const u32 sq_x = add(mul(au, au), mul(av, av));
        const u32 sq_y = add(mul(bu, bu), mul(bv, bv));
        const u32 determinant = fabs(sub(mul(au, bv), mul(av, bu)));
        const u32 sq_major = fmax(sq_x, sq_y);
        const u32 length_major = fsqrt(sq_major);
        const u32 ratio = div(sq_major, determinant);
        const u32 clamped_ratio = lt(max_aniso, ratio);
        // Ratio clamped: the minor axis preserves the area; else the area
        // gives it. A zero determinant (a degenerate footprint) is a
        // clamped, infinite ratio.
        const u32 length_minor = sel(lor(clamped_ratio, lnot(lt(zero, determinant))),
                                     div(length_major, max_aniso), div(determinant, length_major));
        lod = sel(anisotropic_filter, ext1(kGlslStd450Log2, length_minor), lod);
    }
    const u32 bias = b.emit_bitcast(f, sampler_state_word(sampler, kSamplerStateBiasWord));
    const u32 min_lod = b.emit_bitcast(f, sampler_state_word(sampler, kSamplerStateMinLodWord));
    const u32 max_lod = b.emit_bitcast(f, sampler_state_word(sampler, kSamplerStateMaxLodWord));
    const u32 unclamped = add(lod, bias);
    const u32 top = b.emit_convert_s_to_f(
        f, b.emit_isub(m_type_int, b.emit_image_query_levels(m_type_int, image), b.emit_constant_int(m_type_int, 1u)));
    const u32 clamped = fmin(fmax(fmax(min_lod, fmin(max_lod, unclamped)), k(0.0f)), top);
    const u32 zero = k(0.0f);
    u32 result = b.emit_composite_construct(m_type_vec4, {clamped, unclamped, zero, zero});
    result = apply_swizzle(result, resource.swizzle);
    return store_vec4(dst, result);
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
