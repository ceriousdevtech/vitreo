// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_sm1.cpp
/// @brief ps_1_x dependent reads, texture-matrix rows and bump mapping.
///
/// Contracts: Microsoft Learn, "ps_1_1 - ps_1_3 instructions" and "ps_1_4
/// instructions" (texm3x2pad, texm3x2tex, texm3x2depth, texm3x3pad,
/// texm3x3tex, texm3x3, texm3x3spec, texm3x3vspec, texbem, texbeml,
/// texreg2ar, texreg2gb, texreg2rgb, texdp3, texdp3tex, texdepth, bem).
///
/// Notation below: texcoord(m) is the interpolated texture coordinate set m,
/// t(n) a texture register, and stage m's bump-environment state
/// D3DTSS_BUMPENVMAT00..11 / BUMPENVLSCALE / BUMPENVLOFFSET reaches the shader
/// as cb.bump_env[2m] = (MAT00, MAT01, MAT10, MAT11) and
/// cb.bump_env[2m+1] = (LSCALE, LOFFSET, 0, 0).

#include "sm3_to_msl_internal.hpp"

#include <string>

namespace vitreo::graphics::sm3_msl_detail {

std::string Translator::sm1_sample(u32 stage, const std::string& coord3) const {
    const std::string index = std::to_string(stage);
    const u8 dim = stage < m_stage_dim.size() ? m_stage_dim[stage] : 0u;
    // A 2D texture takes (u, v); a cube or volume texture the whole vector.
    const std::string coord =
        (dim == 3 || dim == 4) ? "(" + coord3 + ")" : "(" + coord3 + ").xy";
    return "tex" + index + ".sample(samp" + index + ", " + coord + ")";
}

bool Translator::translate_sm1_texture_op(u32 opcode) {
    Parameter dst;
    if (!has(1)) return fail("a truncated instruction");
    dst = decode_destination(take());

    // texdepth r5 (ps_1_4): depth = r5.r / r5.g, 1.0 when r5.g is 0.
    if (opcode == OP_TEXDEPTH) {
        if (dst.type != REG_TEMP || dst.index != 5) {
            return fail("texdepth destination other than r5");
        }
        std::string r5;
        if (!destination_name(dst, r5)) return false;
        m_depth_written = true;
        m_body << "    out.depth = (" << r5 << ").y == 0.0 ? 1.0 : (" << r5
               << ").x / (" << r5 << ").y;\n";
        return true;
    }

    Parameter src;
    if (!has(1)) return fail("a truncated texture instruction");
    src = decode_source(take());

    // bem dst.rg, src0, src1 (ps_1_4): a perturbed coordinate written to a
    // temporary, using the matrix of the stage numbered like dst.
    if (opcode == OP_BEM) {
        Parameter src1;
        if (!has(1)) return fail("a truncated instruction");
        src1 = decode_source(take());
        if (dst.type != REG_TEMP || dst.index >= 8) {
            return fail("bem destination other than r0-r5");
        }
        std::string s0;
        std::string s1;
        if (!source_expression(src, 0x3, s0) ||
            !source_expression(src1, 0x3, s1)) {
            return false;
        }
        m_result.uses_bump_env = true;
        const std::string be = "cb.bump_env[" + std::to_string(2 * dst.index) + "]";
        Parameter rg = dst;
        rg.write_mask &= 0x3u;
        return assign(rg, "float4((" + s0 + ").x + " + be + ".x * (" + s1 +
                              ").x + " + be + ".z * (" + s1 + ").y, (" + s0 +
                              ").y + " + be + ".y * (" + s1 + ").x + " + be +
                              ".w * (" + s1 + ").y, 0.0, 0.0)" +
                              mask_to_string(rg.write_mask));
    }

    if (dst.type != REG_ADDR) {
        return fail("SM1 texture op destination other than a texture register");
    }
    const u32 m = dst.index;
    if (m >= m_result.samplers_used.size()) {
        return fail("a sampler register above s15");
    }
    m_texregs_used.insert(m);
    m_texcoords_written.insert(m);
    const std::string texcoord = "in.texcoord" + std::to_string(m);
    std::string t_rgb;
    if (!source_expression(src, 0x7, t_rgb)) return false;
    std::string t_rgba;
    if (!source_expression(src, 0xF, t_rgba)) return false;
    const auto sample_into_dst = [&](const std::string& coord3) {
        m_result.samplers_used[m] = true;
        return assign(dst, sm1_sample(m, coord3) + mask_to_string(dst.write_mask));
    };

    // One row of a texture-coordinate matrix: dot(texcoord(m).xyz, t(n).rgb).
    const std::string row = "dot(" + texcoord + ".xyz, (" + t_rgb + "))";
    const std::string matrix = "sm1_matrix" + std::to_string(m_sm1_matrix_sequence);

    switch (opcode) {
    case OP_TEXM3X2PAD:
    case OP_TEXM3X3PAD: {
        const u32 limit = opcode == OP_TEXM3X2PAD ? 1u : 2u;
        if (m_sm1_matrix_rows == 0) {
            ++m_sm1_matrix_sequence;
            m_body << "    float3 sm1_matrix" << m_sm1_matrix_sequence
                   << " = float3(0.0);\n";
        } else if (m_sm1_matrix_rows >= limit) {
            return fail("a texture matrix with too many pad rows");
        }
        static const char* kRow[] = {".x", ".y", ".z"};
        m_body << "    sm1_matrix" << m_sm1_matrix_sequence
               << kRow[m_sm1_matrix_rows] << " = dot(" << texcoord << ".xyz, ("
               << t_rgb << "));\n";
        ++m_sm1_matrix_rows;
        return true;
    }
    case OP_TEXM3X2TEX:
    case OP_TEXM3X2DEPTH: {
        if (m_sm1_matrix_rows != 1) {
            return fail("texm3x2tex/texm3x2depth without one texm3x2pad");
        }
        m_sm1_matrix_rows = 0;
        m_body << "    " << matrix << ".y = " << row << ";\n";
        if (opcode == OP_TEXM3X2DEPTH) {
            // z from the pad row, w from this one; depth z/w, 1.0 when w = 0.
            m_depth_written = true;
            m_body << "    out.depth = " << matrix << ".y == 0.0 ? 1.0 : "
                   << matrix << ".x / " << matrix << ".y;\n";
            return assign(dst, "float4(" + matrix + ".xy, 0.0, 1.0)" +
                                   mask_to_string(dst.write_mask));
        }
        return sample_into_dst("float3(" + matrix + ".xy, 0.0)");
    }
    case OP_TEXM3X3TEX:
    case OP_TEXM3X3:
    case OP_TEXM3X3SPEC:
    case OP_TEXM3X3VSPEC: {
        if (m_sm1_matrix_rows != 2) {
            return fail("a texm3x3 terminal op without two texm3x3pad rows");
        }
        m_sm1_matrix_rows = 0;
        m_body << "    " << matrix << ".z = " << row << ";\n";
        if (opcode == OP_TEXM3X3) {
            return assign(dst, "float4(" + matrix + ", 1.0)" +
                                   mask_to_string(dst.write_mask));
        }
        if (opcode == OP_TEXM3X3TEX) return sample_into_dst(matrix);
        // Reflect the eye ray about the normal N = the matrix product:
        // R = 2 * N * (N.E) / (N.N) - E.
        std::string eye;
        if (opcode == OP_TEXM3X3SPEC) {
            Parameter constant;
            if (!has(1)) return fail("a truncated instruction");
            constant = decode_source(take());
            if (constant.type != REG_CONST) {
                return fail("texm3x3spec eye vector not in a constant register");
            }
            if (!source_expression(constant, 0x7, eye)) return false;
        } else {
            // texm3x3vspec: the eye ray is the w of the three rows' texture
            // coordinates, texcoord(m-2..m).w.
            if (m < 2) return fail("texm3x3vspec below t2");
            eye = "float3(in.texcoord" + std::to_string(m - 2) + ".w, in.texcoord" +
                  std::to_string(m - 1) + ".w, " + texcoord + ".w)";
            m_texcoords_written.insert(m - 2);
            m_texcoords_written.insert(m - 1);
        }
        const std::string e = "(" + eye + ")";
        return sample_into_dst("2.0 * " + matrix + " * dot(" + matrix + ", " + e +
                               ") / dot(" + matrix + ", " + matrix + ") - " + e);
    }
    case OP_TEXBEM:
    case OP_TEXBEML: {
        // u' = u + MAT00 * t(n).r + MAT10 * t(n).g
        // v' = v + MAT01 * t(n).r + MAT11 * t(n).g
        m_result.uses_bump_env = true;
        const std::string be = "cb.bump_env[" + std::to_string(2 * m) + "]";
        const std::string uv = sm1_sample_uv(m, texcoord);
        const std::string t = "(" + t_rgba + ")";
        const std::string coord = "float3(" + uv + " + float2(" + be + ".x * " + t +
                                  ".x + " + be + ".z * " + t + ".y, " + be + ".y * " +
                                  t + ".x + " + be + ".w * " + t + ".y), 0.0)";
        m_result.samplers_used[m] = true;
        std::string sampled = sm1_sample(m, coord);
        if (opcode == OP_TEXBEML) {
            // The colour is scaled by saturate(t(n).b * LSCALE + LOFFSET);
            // alpha is not.
            const std::string lum = "cb.bump_env[" + std::to_string(2 * m + 1) + "]";
            sampled = "(" + sampled + " * float4(float3(saturate(" + t + ".z * " +
                      lum + ".x + " + lum + ".y)), 1.0))";
        }
        return assign(dst, sampled + mask_to_string(dst.write_mask));
    }
    case OP_TEXREG2AR:
        return sample_into_dst("float3((" + t_rgba + ").w, (" + t_rgba + ").x, 0.0)");
    case OP_TEXREG2GB:
        return sample_into_dst("float3((" + t_rgba + ").y, (" + t_rgba + ").z, 0.0)");
    case OP_TEXREG2RGB:
        return sample_into_dst("(" + t_rgb + ")");
    case OP_TEXDP3TEX:
        return sample_into_dst("float3(" + row + ", 0.0, 0.0)");
    case OP_TEXDP3:
        return assign(dst, "float4(" + row + ")" + mask_to_string(dst.write_mask));
    default:
        return fail("opcode " + std::to_string(opcode));
    }
}

} // namespace vitreo::graphics::sm3_msl_detail
