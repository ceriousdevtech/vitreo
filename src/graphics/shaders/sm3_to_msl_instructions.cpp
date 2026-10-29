// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_instructions.cpp
/// @brief The SM1-3 opcode switch.

#include "sm3_to_msl_internal.hpp"

#include <string>

namespace vitreo::graphics::sm3_msl_detail {

bool Translator::parse_instruction_body(u32 token, bool& finished) {
    const u32 opcode = token & OPCODE_MASK;
    const u32 control = (token >> CONTROL_SHIFT) & CONTROL_MASK;
    (void)finished;

    if (m_expect_label_or_end && opcode != OP_LABEL) {
        return fail("code after ret must begin a new label");
    }

    // Shader model 1 has no length field, so operand counts come from the
    // opcode alone; every opcode below reads exactly what it needs.
    const u32 declared_length = (token >> LENGTH_SHIFT) & LENGTH_MASK;
    (void)declared_length;

    const auto read_dst = [&](Parameter& out) {
        if (!has(1)) return false;
        out = decode_destination(take());
        return true;
    };
    const auto read_src = [&](Parameter& out) {
        if (!has(1)) return false;
        out = decode_source(take());
        // vs_2_0+ encodes the address register in a following source token
        // when relative addressing is set; vs_1_1 uses a0.x implicitly.
        // Inside loop…endloop the address token is aL (REG_LOOP).
        if (out.relative && m_result.stage == ShaderStage::Vertex &&
            m_result.major >= 2) {
            if (!has(1)) return false;
            const Parameter addr = decode_source(take());
            if (addr.type == REG_LOOP) {
                out.relative_loop = true;
                out.relative_component = 0;
            } else if (addr.type == REG_ADDR) {
                out.relative_loop = false;
                out.relative_component = addr.swizzle & 0x3u;
            } else {
                return fail("relative addressing without an address register");
            }
        }
        return true;
    };

    switch (opcode) {
    case OP_NOP:
        return true;
    case OP_DCL:
        return handle_declaration(control);
    case OP_DEF:
        return handle_define();
    case OP_DEFI:
        return handle_define_int();
    case OP_DEFB:
        return handle_define_bool();

    // -- unary ---------------------------------------------------------------
    case OP_MOV:
    case OP_FRC:
    case OP_ABS:
    case OP_RCP:
    case OP_RSQ:
    case OP_EXP:
    case OP_LOG:
    case OP_EXPP:
    case OP_LOGP:
    case OP_NRM:
    case OP_MOVA: {
        Parameter dst;
        Parameter src;
        if (!read_dst(dst) || !read_src(src)) {
            return fail("a truncated instruction");
        }
        if (opcode == OP_MOVA) {
            // mova always targets the address register; the round-to-nearest
            // conversion is applied by the a0 store in assign().
            if (dst.type != REG_ADDR) {
                return fail("mova to a non-address register");
            }
            std::string operand;
            if (!source_expression(src, dst.write_mask, operand)) return false;
            return assign(dst, operand);
        }
        if (opcode == OP_NRM) {
            // D3D9 computes the reciprocal length from xyz and multiplies all
            // four source components by it. Preserve w rather than replacing
            // it with 1, and use FLT_MAX for a zero-length xyz vector as the
            // instruction contract specifies.
            std::string operand;
            if (!source_expression(src, 0xF, operand)) return false;
            const std::string xyz = "(" + operand + ").xyz";
            const std::string length_squared =
                "dot(" + xyz + ", " + xyz + ")";
            const std::string reciprocal_length =
                "((" + length_squared + " == 0.0f) ? "
                "3.402823466e+38f : rsqrt(" + length_squared + "))";
            return assign(dst, "((" + operand + ") * " +
                                   reciprocal_length + ")" +
                                   mask_to_string(dst.write_mask));
        }
        if (opcode == OP_RCP || opcode == OP_RSQ || opcode == OP_EXP ||
            opcode == OP_LOG || opcode == OP_EXPP || opcode == OP_LOGP) {
            // These are scalar: they read one component and replicate.
            Parameter scalar_src = src;
            std::string operand;
            if (!source_expression(scalar_src, 0x1, operand)) return false;
            std::string call;
            switch (opcode) {
            case OP_RCP: call = "(1.0 / " + operand + ")"; break;
            case OP_RSQ: call = "rsqrt(abs(" + operand + "))"; break;
            case OP_EXP:
            case OP_EXPP: call = "exp2(" + operand + ")"; break;
            default:
                // log and logp are undefined at zero in D3D9; guarding the
                // magnitude keeps the result finite instead of NaN.
                call = "log2(max(abs(" + operand + "), 1e-30))";
                break;
            }
            return assign_scalar(dst, call);
        }
        std::string operand;
        if (!source_expression(src, dst.write_mask, operand)) return false;
        switch (opcode) {
        case OP_MOV: return assign(dst, operand);
        case OP_FRC: return assign(dst, "fract(" + operand + ")");
        case OP_ABS: return assign(dst, "abs(" + operand + ")");
        default: break;
        }
        return fail("an unhandled unary opcode");
    }

    case OP_SGN: {
        // Learn: sgn dest, src0, src1, src2. dest = sign(src0); src1 and
        // src2 are scratch temps the driver may clobber. Consume all three
        // sources so the token stream stays aligned — reading only src0
        // (the previous path) desynchronises every later instruction.
        Parameter dst;
        Parameter src0;
        Parameter src1;
        Parameter src2;
        if (!read_dst(dst) || !read_src(src0) || !read_src(src1) ||
            !read_src(src2)) {
            return fail("a truncated instruction");
        }
        if (src1.type != REG_TEMP || src2.type != REG_TEMP) {
            return fail("sgn scratch sources must be temporary registers");
        }
        (void)src1;
        (void)src2;
        std::string operand;
        if (!source_expression(src0, dst.write_mask, operand)) return false;
        return assign(dst, "sign(" + operand + ")");
    }

    // -- binary --------------------------------------------------------------
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_MIN:
    case OP_MAX:
    case OP_SLT:
    case OP_SGE:
    case OP_POW:
    case OP_CRS:
    case OP_DP3:
    case OP_DP4: {
        Parameter dst;
        Parameter a;
        Parameter b;
        if (!read_dst(dst) || !read_src(a) || !read_src(b)) {
            return fail("a truncated instruction");
        }
        if (opcode == OP_DP3 || opcode == OP_DP4) {
            const u32 mask = opcode == OP_DP3 ? 0x7u : 0xFu;
            std::string left;
            std::string right;
            if (!source_expression(a, mask, left) ||
                !source_expression(b, mask, right)) {
                return false;
            }
            return assign_scalar(dst, "dot(" + left + ", " + right + ")");
        }
        if (opcode == OP_CRS) {
            std::string left;
            std::string right;
            if (!source_expression(a, 0x7, left) ||
                !source_expression(b, 0x7, right)) {
                return false;
            }
            if (mask_component_count(dst.write_mask) != 3) {
                return fail("a cross product written to a partial mask");
            }
            return assign(dst, "cross(" + left + ", " + right + ")");
        }
        if (opcode == OP_POW) {
            std::string left;
            std::string right;
            if (!source_expression(a, 0x1, left) ||
                !source_expression(b, 0x1, right)) {
                return false;
            }
            return assign_scalar(dst,
                                 "pow(abs(" + left + "), " + right + ")");
        }
        std::string left;
        std::string right;
        if (!source_expression(a, dst.write_mask, left) ||
            !source_expression(b, dst.write_mask, right)) {
            return false;
        }
        switch (opcode) {
        case OP_ADD: return assign(dst, left + " + " + right);
        case OP_SUB: return assign(dst, left + " - " + right);
        case OP_MUL: return assign(dst, left + " * " + right);
        case OP_MIN: return assign(dst, "min(" + left + ", " + right + ")");
        case OP_MAX: return assign(dst, "max(" + left + ", " + right + ")");
        case OP_SLT:
        case OP_SGE: {
            const std::string type =
                vector_type(mask_component_count(dst.write_mask));
            const std::string comparison = opcode == OP_SLT ? " < " : " >= ";
            return assign(dst, "select(" + type + "(0.0), " + type +
                                   "(1.0), " + left + comparison + right +
                                   ")");
        }
        default: break;
        }
        return fail("an unhandled binary opcode");
    }

    // -- ternary -------------------------------------------------------------
    case OP_MAD:
    case OP_LRP:
    case OP_CMP:
    case OP_CND:
    case OP_DP2ADD: {
        Parameter dst;
        Parameter a;
        Parameter b;
        Parameter c;
        if (!read_dst(dst) || !read_src(a) || !read_src(b) || !read_src(c)) {
            return fail("a truncated instruction");
        }
        if (opcode == OP_DP2ADD) {
            std::string left;
            std::string right;
            std::string add;
            if (!source_expression(a, 0x3, left) ||
                !source_expression(b, 0x3, right) ||
                !source_expression(c, 0x1, add)) {
                return false;
            }
            return assign_scalar(
                dst, "(dot(" + left + ", " + right + ") + " + add + ")");
        }
        std::string x;
        std::string y;
        std::string z;
        if (!source_expression(a, dst.write_mask, x) ||
            !source_expression(b, dst.write_mask, y) ||
            !source_expression(c, dst.write_mask, z)) {
            return false;
        }
        switch (opcode) {
        case OP_MAD: return assign(dst, x + " * " + y + " + " + z);
        case OP_LRP:
            // D3D9 lerps from the third source toward the second.
            return assign(dst, "mix(" + z + ", " + y + ", " + x + ")");
        case OP_CMP:
            return assign(dst, "select(" + z + ", " + y + ", " + x +
                                   " >= 0.0)");
        case OP_CND:
            return assign(dst, "select(" + z + ", " + y + ", " + x +
                                   " > 0.5)");
        default: break;
        }
        return fail("an unhandled ternary opcode");
    }

    // -- matrix multiplies ---------------------------------------------------
    case OP_M4X4:
    case OP_M4X3:
    case OP_M3X4:
    case OP_M3X3:
    case OP_M3X2: {
        Parameter dst;
        Parameter vector;
        Parameter matrix;
        if (!read_dst(dst) || !read_src(vector) || !read_src(matrix)) {
            return fail("a truncated instruction");
        }
        u32 vector_size = 4;
        u32 row_count = 4;
        switch (opcode) {
        case OP_M4X4: vector_size = 4; row_count = 4; break;
        case OP_M4X3: vector_size = 4; row_count = 3; break;
        case OP_M3X4: vector_size = 3; row_count = 4; break;
        case OP_M3X3: vector_size = 3; row_count = 3; break;
        default:      vector_size = 3; row_count = 2; break;
        }
        const u32 vector_mask = vector_size == 4 ? 0xFu : 0x7u;
        std::string operand;
        if (!source_expression(vector, vector_mask, operand)) return false;

        // Each matrix row is a consecutive register starting at the operand.
        std::string name;
        if (!destination_name(dst, name)) return false;
        constexpr char components[] = "xyzw";
        for (u32 row = 0; row < row_count; ++row) {
            Parameter row_param = matrix;
            row_param.index = matrix.index + row;
            std::string row_expression;
            if (!source_expression(row_param, vector_mask, row_expression)) {
                return false;
            }
            const bool saturate =
                (dst.result_modifier & DSTMOD_SATURATE) != 0;
            m_body << "    " << name << "." << components[row] << " = "
                   << (saturate ? "saturate(" : "") << "dot(" << operand
                   << ", " << row_expression << ")"
                   << (saturate ? ")" : "") << ";\n";
        }
        return true;
    }

    // -- texture -------------------------------------------------------------
    case OP_TEX: {
        // Instruction controls: D3DSI_TEXLD_PROJECT (texldp) and
        // D3DSI_TEXLD_BIAS (texldb). Without the projective divide, projected
        // lightmaps sample as a flat colour.
        constexpr u32 kTexldProject = 0x00010000u;
        constexpr u32 kTexldBias = 0x00020000u;
        const bool projected = (token & kTexldProject) != 0;
        const bool lod_bias = (token & kTexldBias) != 0;

        Parameter dst;
        if (!read_dst(dst)) return fail("a truncated instruction");
        if (m_result.major >= 2) {
            Parameter coordinate;
            Parameter sampler;
            if (!read_src(coordinate) || !read_src(sampler)) {
                return fail("a truncated texture instruction");
            }
            if (sampler.type != REG_SAMPLER) {
                return fail("a texture read from a non-sampler register");
            }
            if (sampler.index >= m_result.samplers_used.size()) {
                return fail("a sampler register above s15");
            }
            m_result.samplers_used[sampler.index] = true;
            const std::string index = std::to_string(sampler.index);
            const std::string mask = mask_to_string(dst.write_mask);
            // dcl_cube / dcl_volume require .xyz sample coords (texld docs);
            // 2D keeps .xy. texld always supplies float4 coordinates.
            const u8 dim = m_sampler_dim[sampler.index];
            const bool cube_or_volume = dim == 3 || dim == 4;
            const u32 coord_mask = cube_or_volume ? 0x7u : 0x3u;
            const char* coord_swizzle = cube_or_volume ? ".xyz" : ".xy";
            if (projected || lod_bias) {
                std::string tc;
                if (!source_expression(coordinate, 0xF, tc)) return false;
                if (projected) {
                    return assign(
                        dst,
                        "tex" + index + ".sample(samp" + index + ", (" + tc +
                            ")" + coord_swizzle + " / max(abs((" + tc +
                            ").w), 1e-6))" + mask);
                }
                return assign(dst,
                              "tex" + index + ".sample(samp" + index + ", (" +
                                  tc + ")" + coord_swizzle + ", bias((" + tc +
                                  ").w))" + mask);
            }
            std::string uv;
            if (!source_expression(coordinate, coord_mask, uv)) return false;
            return assign(dst, "tex" + index + ".sample(samp" + index + ", " +
                                   uv + ")" + mask);
        }
        // SM1.1–1.3: `tex tn` (dest-only). SM1.4 / some compilers: `texld
        // dest, src` even when the version token still says 1.1. Parameter
        // tokens have bit 31 set; instruction tokens do not, so peeking
        // distinguishes the two forms without trusting the length field.
        //
        // Projective divide for ps_1_1–1_3 comes from runtime
        // D3DTSS_TEXTURETRANSFORMFLAGS (D3DTTFF_PROJECTED), not texldp.
        const bool has_coord =
            has(1) && (m_tokens[m_position] & 0x80000000u) != 0;
        u32 stage = dst.index;
        std::string tc4;
        if (has_coord) {
            Parameter coordinate;
            if (!read_src(coordinate)) {
                return fail("a truncated texture instruction");
            }
            if (coordinate.type == REG_ADDR) stage = coordinate.index;
            if (!source_expression(coordinate, 0xF, tc4)) return false;
        } else if (dst.type == REG_ADDR) {
            stage = dst.index;
            m_texregs_used.insert(stage);
            m_texcoords_written.insert(stage);
            tc4 = "t" + std::to_string(stage);
        } else {
            // `tex rN` with no source: sample stage N with varying N.
            m_texcoords_written.insert(dst.index);
            stage = dst.index;
            tc4 = "in.texcoord" + std::to_string(dst.index);
        }
        if (stage >= m_result.samplers_used.size()) {
            return fail("a sampler register above s15");
        }
        m_result.samplers_used[stage] = true;
        const std::string index = std::to_string(stage);
        std::string uv = sm1_sample_uv(stage, tc4);
        // Opcode texldp bit still forces ÷w when set (rare on SM1).
        if (projected && (m_stage_ttf[stage] & 0x100u) == 0) {
            uv = "((" + tc4 + ").xy / max(abs((" + tc4 + ").w), 1e-6))";
        }
        return assign(dst, "tex" + index + ".sample(samp" + index + ", " + uv +
                               ")" + mask_to_string(dst.write_mask));
    }
    case OP_TEXKILL: {
        // The bytecode encodes this as a destination parameter even though
        // the instruction only reads the register. Do not interpret its
        // write-mask bits as a source swizzle.
        Parameter coordinate;
        if (!read_dst(coordinate)) return fail("a truncated instruction");
        std::string expression;
        if (!source_expression(coordinate, 0x7, expression)) return false;
        m_body << "    if (any(" << expression
               << " < float3(0.0))) discard_fragment();\n";
        return true;
    }
    case OP_TEXCOORD: {
        // SM1.1–1.3: `texcoord tn`. SM1.4: `texcrd dest, src`.
        if (m_result.major >= 2) {
            return fail("texcoord outside shader model 1");
        }
        Parameter dst;
        if (!read_dst(dst)) return fail("a truncated instruction");
        if (m_result.minor >= 4) {
            Parameter src;
            if (!read_src(src)) return fail("a truncated instruction");
            std::string expression;
            if (!source_expression(src, dst.write_mask, expression)) {
                return false;
            }
            return assign(dst, expression);
        }
        if (dst.type != REG_ADDR && dst.type != REG_TEMP) {
            return fail("texcoord destination register type " +
                        std::to_string(dst.type));
        }
        m_texcoords_written.insert(dst.index);
        if (dst.type == REG_ADDR) m_texregs_used.insert(dst.index);
        return assign(dst, "in.texcoord" + std::to_string(dst.index) +
                               mask_to_string(dst.write_mask));
    }
    case OP_TEXM3X2PAD:
    case OP_TEXM3X2TEX:
    case OP_TEXM3X3PAD:
    case OP_TEXM3X3TEX:
    case OP_TEXM3X3SPEC:
    case OP_TEXM3X3VSPEC:
    case OP_TEXBEM:
    case OP_TEXBEML:
    case OP_TEXREG2AR:
    case OP_TEXREG2GB:
    case OP_TEXREG2RGB:
    case OP_TEXDP3:
    case OP_TEXDP3TEX:
    case OP_TEXM3X2DEPTH:
    case OP_TEXDEPTH:
    case OP_TEXM3X3:
    case OP_BEM: {
        // SM1 dependent-read / bump / matrix tex ops. The bump matrix lives
        // in D3DTSS state that this translator does not yet bind as a
        // uniform, so use an identity transform: copy the source texreg
        // and optionally sample. That is enough for titles to create the
        // shader and draw; lighting will be wrong until matrices are wired.
        if (m_result.major >= 2) {
            return fail("opcode " + std::to_string(opcode));
        }
        Parameter dst;
        if (!read_dst(dst)) return fail("a truncated instruction");
        Parameter src;
        const bool needs_src =
            opcode != OP_TEXDEPTH; // texdepth is dest-only
        if (needs_src && !read_src(src)) {
            return fail("a truncated texture instruction");
        }
        std::string src_xy;
        std::string src_xyz;
        if (needs_src) {
            if (!source_expression(src, 0x3, src_xy) ||
                !source_expression(src, 0x7, src_xyz)) {
                return false;
            }
        }
        const bool sample =
            opcode == OP_TEXM3X2TEX || opcode == OP_TEXM3X3TEX ||
            opcode == OP_TEXM3X3SPEC || opcode == OP_TEXM3X3VSPEC ||
            opcode == OP_TEXBEM || opcode == OP_TEXBEML ||
            opcode == OP_TEXREG2AR || opcode == OP_TEXREG2GB ||
            opcode == OP_TEXREG2RGB || opcode == OP_TEXDP3TEX ||
            opcode == OP_TEXM3X2DEPTH;
        if (dst.type == REG_ADDR) {
            m_texregs_used.insert(dst.index);
            m_texcoords_written.insert(dst.index);
        }
        if (sample) {
            const u32 stage = dst.index;
            if (stage >= m_result.samplers_used.size()) {
                return fail("a sampler register above s15");
            }
            m_result.samplers_used[stage] = true;
            const std::string index = std::to_string(stage);
            const std::string coord =
                needs_src ? src_xy : ("t" + index + ".xy");
            return assign(dst, "tex" + index + ".sample(samp" + index +
                                   ", " + coord + ")" +
                                   mask_to_string(dst.write_mask));
        }
        // Pad / project ops: write the (identity-transformed) coordinate.
        if (opcode == OP_TEXM3X3PAD || opcode == OP_TEXM3X3 ||
            opcode == OP_TEXDP3) {
            Parameter coordinate_dst = dst;
            coordinate_dst.write_mask &= 0x7u;
            return assign(coordinate_dst, src_xyz);
        }
        Parameter coordinate_dst = dst;
        coordinate_dst.write_mask &= 0x3u;
        return assign(coordinate_dst, src_xy);
    }
    case OP_SETP: {
        Parameter dst;
        Parameter src0;
        Parameter src1;
        if (!read_dst(dst) || !read_src(src0) || !read_src(src1)) {
            return fail("a truncated instruction");
        }
        if (dst.type != REG_PREDICATE) {
            return fail("setp destination must be a predicate register");
        }
        m_predicates_used.insert(dst.index);
        std::string left;
        std::string right;
        if (!source_expression(src0, dst.write_mask, left) ||
            !source_expression(src1, dst.write_mask, right)) {
            return false;
        }
        const char* op = nullptr;
        switch (control) {
        case 1: op = ">"; break;   // GT
        case 2: op = "=="; break;  // EQ
        case 3: op = ">="; break;  // GE
        case 4: op = "<"; break;   // LT
        case 5: op = "!="; break;  // NE
        case 6: op = "<="; break;  // LE
        default:
            return fail("setp comparison control " +
                        std::to_string(control));
        }
        m_body << "    p" << dst.index << mask_to_string(dst.write_mask)
               << " = (" << left << " " << op << " " << right << ");\n";
        return true;
    }

    // -- everything else -----------------------------------------------------
    case OP_LIT: {
        // lit dst, src — lighting coefficients (MSDN lit - vs):
        //   dest.x = 1
        //   dest.y = max(src.x, 0)
        //   dest.z = (src.x > 0) ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0
        //   dest.w = 1
        Parameter dst;
        Parameter src;
        if (!read_dst(dst) || !read_src(src)) {
            return fail("a truncated instruction");
        }
        std::string sx;
        std::string sy;
        std::string sw;
        if (!source_expression(src, 0x1, sx) ||
            !source_expression(src, 0x2, sy) ||
            !source_expression(src, 0x8, sw)) {
            return false;
        }
        std::string expr =
            "float4(1.0, max(" + sx + ", 0.0), (" + sx +
            " > 0.0) ? pow(max(" + sy + ", 0.0), clamp(" + sw +
            ", -128.0, 128.0)) : 0.0, 1.0)";
        if (dst.write_mask != 0xF) {
            expr += mask_to_string(dst.write_mask);
        }
        return assign(dst, expr);
    }

    case OP_DST: {
        // Learn: dst - vs. dest = (1, src0.y*src1.y, src0.z, src1.w).
        Parameter dst;
        Parameter src0;
        Parameter src1;
        if (!read_dst(dst) || !read_src(src0) || !read_src(src1)) {
            return fail("a truncated instruction");
        }
        std::string y0;
        std::string y1;
        std::string z0;
        std::string w1;
        if (!source_expression(src0, 0x2, y0) ||
            !source_expression(src1, 0x2, y1) ||
            !source_expression(src0, 0x4, z0) ||
            !source_expression(src1, 0x8, w1)) {
            return false;
        }
        return assign(dst, "float4(1.0, (" + y0 + ") * (" + y1 + "), " + z0 +
                               ", " + w1 + ")");
    }
    case OP_SINCOS: {
        // Learn: sincos - vs/ps. SM3 is `sincos dst.{x|y|xy}, src0.{x|y|z|w}`;
        // SM2 also takes two Taylor-series constant sources that Metal's
        // sin/cos make unnecessary — still consume them so the stream aligns.
        // dest.x = cos(V), dest.y = sin(V); other components untouched (SM3)
        // or undefined (SM2). Destination must be a temporary.
        Parameter dst;
        Parameter src0;
        if (!read_dst(dst) || !read_src(src0)) {
            return fail("a truncated instruction");
        }
        if (dst.type != REG_TEMP) {
            return fail("sincos destination must be a temporary register");
        }
        if (m_result.major < 3) {
            Parameter src1;
            Parameter src2;
            if (!read_src(src1) || !read_src(src2)) {
                return fail("a truncated instruction");
            }
            (void)src1;
            (void)src2;
        }
        const u32 mask = dst.write_mask;
        if (mask != 0x1 && mask != 0x2 && mask != 0x3) {
            return fail("sincos write mask (must be .x, .y, or .xy)");
        }
        std::string angle;
        if (!source_expression(src0, 0x1, angle)) return false;
        if (mask == 0x1) return assign(dst, "cos(" + angle + ")");
        if (mask == 0x2) return assign(dst, "sin(" + angle + ")");
        return assign(dst,
                      "float2(cos(" + angle + "), sin(" + angle + "))");
    }
    case OP_IF: {
        // Learn: if bool - vs/ps. Source is a constant boolean (b#) or, in
        // SM2+/SM3 dynamic form, a predicate component (p0.x). Optional
        // source-modifier NOT inverts the condition.
        Parameter src;
        if (!read_src(src)) return fail("a truncated instruction");
        std::string cond;
        if (src.type == REG_PREDICATE) {
            m_predicates_used.insert(src.index);
            const char comps[] = {'x', 'y', 'z', 'w'};
            cond = "p" + std::to_string(src.index) + "." +
                   comps[src.swizzle & 0x3u];
        } else if (src.type == REG_CONSTBOOL) {
            if (src.index >= SM3_MAX_BOOL_CONSTANT_REGISTERS) {
                return fail("boolean constant b" +
                            std::to_string(src.index) +
                            " above the D3D9 boolean-constant file");
            }
            const auto defined = m_defined_bool_constants.find(src.index);
            if (defined != m_defined_bool_constants.end()) {
                cond = "defb" + std::to_string(src.index);
            } else {
                note_bool_constant(src.index);
                cond = "(cb.b[" + std::to_string(src.index) + "] != 0)";
            }
        } else {
            return fail("if source must be a bool or predicate register");
        }
        if (src.source_modifier == SRCMOD_NOT) {
            cond = "!(" + cond + ")";
        } else if (src.source_modifier != SRCMOD_NONE) {
            return fail("if source modifier " +
                        std::to_string(src.source_modifier));
        }
        ++m_flow_depth;
        m_body << "    if (" << cond << ") {\n";
        return true;
    }
    case OP_IFC: {
        // Learn: if_comp - vs/ps. Comparison control matches setp (GT/EQ/…).
        Parameter src0;
        Parameter src1;
        if (!read_src(src0) || !read_src(src1)) {
            return fail("a truncated instruction");
        }
        std::string left;
        std::string right;
        if (!source_expression(src0, 0x1, left) ||
            !source_expression(src1, 0x1, right)) {
            return false;
        }
        const char* op = nullptr;
        switch (control) {
        case 1: op = ">"; break;
        case 2: op = "=="; break;
        case 3: op = ">="; break;
        case 4: op = "<"; break;
        case 5: op = "!="; break;
        case 6: op = "<="; break;
        default:
            return fail("ifc comparison control " + std::to_string(control));
        }
        ++m_flow_depth;
        m_body << "    if ((" << left << ") " << op << " (" << right
               << ")) {\n";
        return true;
    }
    case OP_ELSE: {
        if (m_flow_depth == 0) return fail("else without a matching if");
        m_body << "    } else {\n";
        return true;
    }
    case OP_ENDIF: {
        if (m_flow_depth == 0) return fail("endif without a matching if");
        --m_flow_depth;
        m_body << "    }\n";
        return true;
    }
    case OP_LOOP: {
        // Learn: loop aL, i# — i.x = count [0,255], i.y = initial aL,
        // i.z = step [-128,127], i.w unused (0).
        if (m_result.major < 2) {
            return fail("the loop instruction before shader model 2");
        }
        Parameter al;
        Parameter src;
        if (!read_dst(al) || !read_src(src)) {
            return fail("a truncated instruction");
        }
        if (al.type != REG_LOOP) {
            return fail("loop destination must be the aL register");
        }
        std::string i4;
        if (!int_constant_expression(src, i4)) return false;
        const u32 id = m_loop_serial++;
        m_loop_stack.emplace_back(LoopKind::Loop, id);
        m_body << "    {\n"
               << "    const int4 _li" << id << " = " << i4 << ";\n"
               << "    const int _lc" << id << " = clamp(_li" << id
               << ".x, 0, 255);\n"
               << "    int aL = clamp(_li" << id << ".y, 0, 255);\n"
               << "    for (int _lk" << id << " = 0; _lk" << id << " < _lc"
               << id << "; ++_lk" << id << ") {\n";
        return true;
    }
    case OP_ENDLOOP: {
        if (m_loop_stack.empty() ||
            m_loop_stack.back().first != LoopKind::Loop) {
            return fail("endloop without a matching loop");
        }
        const u32 id = m_loop_stack.back().second;
        m_loop_stack.pop_back();
        // aL advances by i.z after each iteration body (Learn: auto-increment).
        m_body << "    aL += _li" << id << ".z;\n"
               << "    }\n"
               << "    }\n";
        return true;
    }
    case OP_REP: {
        // Learn: rep i# — repeat i.x times; aL is not updated.
        if (m_result.major < 2) {
            return fail("the rep instruction before shader model 2");
        }
        Parameter src;
        if (!read_src(src)) return fail("a truncated instruction");
        std::string i4;
        if (!int_constant_expression(src, i4)) return false;
        const u32 id = m_loop_serial++;
        m_loop_stack.emplace_back(LoopKind::Rep, id);
        m_body << "    {\n"
               << "    const int _rc" << id << " = clamp((" << i4
               << ").x, 0, 255);\n"
               << "    for (int _rk" << id << " = 0; _rk" << id << " < _rc"
               << id << "; ++_rk" << id << ") {\n";
        return true;
    }
    case OP_ENDREP: {
        if (m_loop_stack.empty() ||
            m_loop_stack.back().first != LoopKind::Rep) {
            return fail("endrep without a matching rep");
        }
        m_loop_stack.pop_back();
        m_body << "    }\n"
               << "    }\n";
        return true;
    }
    case OP_BREAK: {
        // Learn: break — exit nearest loop/rep at endloop/endrep.
        if (m_loop_stack.empty()) {
            return fail("break outside a loop or rep block");
        }
        m_body << "    break;\n";
        return true;
    }
    case OP_BREAKC: {
        // Learn: break_comp src0, src1 — comparison control matches ifc/setp.
        // Replicate swizzle required; take one scalar component per source.
        if (m_loop_stack.empty()) {
            return fail("breakc outside a loop or rep block");
        }
        Parameter src0;
        Parameter src1;
        if (!read_src(src0) || !read_src(src1)) {
            return fail("a truncated instruction");
        }
        std::string left;
        std::string right;
        if (!source_expression(src0, 0x1, left) ||
            !source_expression(src1, 0x1, right)) {
            return false;
        }
        const char* op = nullptr;
        switch (control) {
        case 1: op = ">"; break;
        case 2: op = "=="; break;
        case 3: op = ">="; break;
        case 4: op = "<"; break;
        case 5: op = "!="; break;
        case 6: op = "<="; break;
        default:
            return fail("breakc comparison control " +
                        std::to_string(control));
        }
        m_body << "    if ((" << left << ") " << op << " (" << right
               << ")) {\n"
               << "        break;\n"
               << "    }\n";
        return true;
    }
    case OP_BREAKP: {
        // Learn: breakp [!]p0.{xyzw} — predicate component (optional NOT).
        if (m_loop_stack.empty()) {
            return fail("breakp outside a loop or rep block");
        }
        Parameter src;
        if (!read_src(src)) return fail("a truncated instruction");
        if (src.type != REG_PREDICATE) {
            return fail("breakp source must be a predicate register");
        }
        m_predicates_used.insert(src.index);
        const char comps[] = {'x', 'y', 'z', 'w'};
        std::string cond = "p" + std::to_string(src.index) + "." +
                           comps[src.swizzle & 0x3u];
        if (src.source_modifier == SRCMOD_NOT) {
            cond = "!(" + cond + ")";
        } else if (src.source_modifier != SRCMOD_NONE) {
            return fail("breakp source modifier " +
                        std::to_string(src.source_modifier));
        }
        m_body << "    if (" << cond << ") {\n"
               << "        break;\n"
               << "    }\n";
        return true;
    }
    case OP_CALL: {
        // Learn: call l# — push return address, jump to label (forward only).
        if (m_result.major < 2) {
            return fail("the call instruction before shader model 2");
        }
        Parameter src;
        if (!read_src(src)) return fail("a truncated instruction");
        if (src.type != REG_LABEL) {
            return fail("call target must be a label register");
        }
        emit_call(src.index);
        return true;
    }
    case OP_CALLNZ: {
        // Learn: callnz l#, [!]b# | callnz l#, [!]p0.replicate
        if (m_result.major < 2) {
            return fail("the callnz instruction before shader model 2");
        }
        Parameter label;
        Parameter cond_src;
        if (!read_src(label) || !read_src(cond_src)) {
            return fail("a truncated instruction");
        }
        if (label.type != REG_LABEL) {
            return fail("callnz target must be a label register");
        }
        std::string cond;
        if (cond_src.type == REG_CONSTBOOL) {
            if (cond_src.index >= SM3_MAX_BOOL_CONSTANT_REGISTERS) {
                return fail("boolean constant b" +
                            std::to_string(cond_src.index) +
                            " above the D3D9 boolean-constant file");
            }
            const auto defined =
                m_defined_bool_constants.find(cond_src.index);
            if (defined != m_defined_bool_constants.end()) {
                cond = "defb" + std::to_string(cond_src.index);
            } else {
                note_bool_constant(cond_src.index);
                cond = "(cb.b[" + std::to_string(cond_src.index) + "] != 0)";
            }
        } else if (cond_src.type == REG_PREDICATE) {
            m_predicates_used.insert(cond_src.index);
            const char comps[] = {'x', 'y', 'z', 'w'};
            cond = "p" + std::to_string(cond_src.index) + "." +
                   comps[cond_src.swizzle & 0x3u];
        } else {
            return fail("callnz condition must be a bool or predicate");
        }
        if (cond_src.source_modifier == SRCMOD_NOT) {
            cond = "!(" + cond + ")";
        } else if (cond_src.source_modifier != SRCMOD_NONE) {
            return fail("callnz source modifier " +
                        std::to_string(cond_src.source_modifier));
        }
        m_body << "    if (" << cond << ") {\n";
        emit_call(label.index);
        m_body << "    }\n";
        return true;
    }
    case OP_RET: {
        // Learn: ret — return from call, or end the shader from main.
        if (m_current_label.has_value()) {
            m_body << "    return;\n";
            flush_body();
            m_current_label.reset();
            m_expect_label_or_end = true;
        } else {
            m_body << "    return out;\n";
        }
        return true;
    }
    case OP_LABEL: {
        // Learn: label l# — subroutine entry (must be after its callers).
        if (m_result.major < 2) {
            return fail("the label instruction before shader model 2");
        }
        Parameter dst;
        if (!read_dst(dst)) return fail("a truncated instruction");
        if (dst.type != REG_LABEL) {
            return fail("label destination must be a label register");
        }
        if (m_labels_defined.count(dst.index) != 0) {
            return fail("duplicate label l" + std::to_string(dst.index));
        }
        if (m_flow_depth != 0 || !m_loop_stack.empty()) {
            return fail("a label inside an open if or loop block");
        }
        flush_body();
        m_uses_subroutines = true;
        m_current_label = dst.index;
        m_labels_defined.insert(dst.index);
        m_expect_label_or_end = false;
        return true;
    }
    case OP_DSX:
    case OP_DSY: {
        // Learn: dsx/dsy - ps. Screen-space derivatives; Metal's dfdx/dfdy.
        Parameter dst;
        Parameter src;
        if (!read_dst(dst) || !read_src(src)) {
            return fail("a truncated instruction");
        }
        if (m_result.stage != ShaderStage::Fragment) {
            return fail("dsx/dsy outside a pixel shader");
        }
        std::string operand;
        if (!source_expression(src, dst.write_mask, operand)) return false;
        const char* fn = opcode == OP_DSX ? "dfdx" : "dfdy";
        return assign(dst, std::string(fn) + "(" + operand + ")");
    }
    case OP_TEXLDD:
    case OP_TEXLDL: {
        // Learn: texldd - ps (explicit ddx/ddy gradients), texldl - ps
        // (LOD in src0.w). Metal: sample(..., gradient*) / sample(..., level).
        Parameter dst;
        Parameter coordinate;
        Parameter sampler;
        if (!read_dst(dst) || !read_src(coordinate) || !read_src(sampler)) {
            return fail("a truncated texture instruction");
        }
        Parameter grad_x;
        Parameter grad_y;
        if (opcode == OP_TEXLDD) {
            if (!read_src(grad_x) || !read_src(grad_y)) {
                return fail("a truncated texture instruction");
            }
        }
        if (opcode == OP_TEXLDD &&
            m_result.stage != ShaderStage::Fragment) {
            return fail("texldd outside a pixel shader");
        }
        if (opcode == OP_TEXLDL &&
            m_result.stage != ShaderStage::Fragment &&
            !(m_result.stage == ShaderStage::Vertex && m_result.major == 3)) {
            return fail("texldl outside a shader-model-3 pixel/vertex shader");
        }
        if (sampler.type != REG_SAMPLER) {
            return fail("a texture read from a non-sampler register");
        }
        if (sampler.index >= m_result.samplers_used.size()) {
            return fail("a sampler register above s15");
        }
        if (m_result.stage == ShaderStage::Vertex && sampler.index >= 4) {
            return fail("a vertex texture sampler register above s3");
        }
        m_result.samplers_used[sampler.index] = true;
        const std::string index = std::to_string(sampler.index);
        const std::string mask = mask_to_string(dst.write_mask);
        const u8 dim = m_sampler_dim[sampler.index];
        const bool cube_or_volume = dim == 3 || dim == 4;
        const u32 coord_mask = cube_or_volume ? 0x7u : 0x3u;
        const char* coord_swizzle = cube_or_volume ? ".xyz" : ".xy";
        if (opcode == OP_TEXLDL) {
            // LOD is the w component of the coordinate register (texldl docs).
            std::string tc;
            if (!source_expression(coordinate, 0xF, tc)) return false;
            return assign(dst,
                          "tex" + index + ".sample(samp" + index + ", (" + tc +
                              ")" + coord_swizzle + ", level((" + tc +
                              ").w))" + mask);
        }
        std::string uv;
        std::string ddx;
        std::string ddy;
        if (!source_expression(coordinate, coord_mask, uv) ||
            !source_expression(grad_x, coord_mask, ddx) ||
            !source_expression(grad_y, coord_mask, ddy)) {
            return false;
        }
        const char* gradient =
            dim == 3 ? "gradientcube" : (dim == 4 ? "gradient3d" : "gradient2d");
        return assign(dst, "tex" + index + ".sample(samp" + index + ", " + uv +
                               ", " + gradient + "(" + ddx + ", " + ddy + "))" +
                               mask);
    }
    default:
        return fail("opcode " + std::to_string(opcode));
    }
}

} // namespace vitreo::graphics::sm3_msl_detail
