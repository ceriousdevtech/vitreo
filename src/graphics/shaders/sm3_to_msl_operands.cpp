// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_operands.cpp
/// @brief SM1-3 source operand references and destination stores.

#include "sm3_to_msl_internal.hpp"

#include <cmath>
#include <string>

namespace vitreo::graphics::sm3_msl_detail {

bool Translator::source_expression(const Parameter& p, u32 mask,
                                   std::string& out) {
    if (p.relative) {
        if (m_result.stage != ShaderStage::Vertex) {
            return fail("relative addressing of a source register");
        }
        if (p.type != REG_CONST && p.type != REG_CONST2 &&
            p.type != REG_CONST3 && p.type != REG_CONST4) {
            return fail("relative addressing of a non-constant register");
        }
    }

    std::string base;
    switch (p.type) {
    case REG_TEMP:
        m_temps_used.insert(p.index);
        base = "r" + std::to_string(p.index);
        break;
    case REG_INPUT:
        if (m_result.stage == ShaderStage::Vertex) {
            base = "in.v" + std::to_string(p.index);
            break;
        }
        // A pixel shader reads varyings through v#. Shader model 3 says which
        // varying with a declaration; shader model 2 has only the two
        // diffuse/specular colour registers.
        {
            const auto declared = m_input_declarations.find(p.index);
            if (declared != m_input_declarations.end() &&
                declared->second.usage == Sm3Usage::TexCoord) {
                m_texcoords_written.insert(declared->second.usage_index);
                base = "in.texcoord" +
                       std::to_string(declared->second.usage_index);
                break;
            }
            const u32 color = declared != m_input_declarations.end()
                                  ? declared->second.usage_index
                                  : p.index;
            m_colors_written.insert(color);
            base = "in.color" + std::to_string(color);
        }
        break;
    case REG_CONST:
    case REG_CONST2:
    case REG_CONST3:
    case REG_CONST4: {
        u32 index = 0;
        if (!constant_index(p, index)) return false;
        if (p.relative) {
            // Relative constant indexing: c[a0.x + n] or c[aL + n] inside
            // loop…endloop (Learn: Address Register). Out-of-range reads
            // return (0,0,0,0), not the last in-range slot.
            const u32 model_max = m_result.major >= 2 ? 256u : 96u;
            if (m_result.float_constant_count < model_max) {
                m_result.float_constant_count = model_max;
            }
            m_relative_const_used = true;
            std::string index_expr;
            if (p.relative_loop) {
                if (!enclosing_loop_counter()) {
                    return fail("aL relative addressing outside a loop");
                }
                index_expr = "aL + " + std::to_string(static_cast<int>(index));
            } else {
                m_address_register_used = true;
                static constexpr char kComp[] = {'x', 'y', 'z', 'w'};
                const char comp =
                    kComp[p.relative_component < 4 ? p.relative_component : 0];
                index_expr = "a0." + std::string(1, comp) + " + " +
                             std::to_string(static_cast<int>(index));
            }
            base = "sm3_c(cb, " + index_expr + ")";
            break;
        }
        const auto defined = m_defined_constants.find(index);
        if (defined != m_defined_constants.end()) {
            base = "def" + std::to_string(index);
        } else {
            note_constant(index);
            base = "cb.c[" + std::to_string(index) + "]";
        }
        break;
    }
    case REG_CONSTINT: {
        // Integer constants are int4; promote to float4 for arithmetic sources
        // (mov/mad/…). Immediate defi takes precedence over the uniform file.
        if (p.index >= SM3_MAX_INT_CONSTANT_REGISTERS) {
            return fail("integer constant i" + std::to_string(p.index) +
                        " above the D3D9 integer-constant file");
        }
        const auto defined = m_defined_int_constants.find(p.index);
        if (defined != m_defined_int_constants.end()) {
            base = "float4(defi" + std::to_string(p.index) + ")";
        } else {
            note_int_constant(p.index);
            base = "float4(cb.i[" + std::to_string(p.index) + "])";
        }
        break;
    }
    case REG_CONSTBOOL: {
        // Bool registers are scalar; replicate into a float4 when used as a
        // generic source. `if b#` resolves them separately without swizzle.
        if (p.index >= SM3_MAX_BOOL_CONSTANT_REGISTERS) {
            return fail("boolean constant b" + std::to_string(p.index) +
                        " above the D3D9 boolean-constant file");
        }
        std::string scalar;
        const auto defined = m_defined_bool_constants.find(p.index);
        if (defined != m_defined_bool_constants.end()) {
            scalar = "defb" + std::to_string(p.index);
        } else {
            note_bool_constant(p.index);
            scalar = "(cb.b[" + std::to_string(p.index) + "] != 0)";
        }
        base = "float4(" + scalar + " ? 1.0 : 0.0)";
        break;
    }
    case REG_MISCTYPE:
        // Learn: vPos (0) and vFace (1) in a pixel shader.
        // vFace sign < 0 is back-facing; compare against 0 only in ps_3_0.
        if (m_result.stage != ShaderStage::Fragment) {
            return fail("a misc-type register outside a pixel shader");
        }
        if (p.index == 0) {
            base = "float4(in.frag_position.xy, 0.0, 0.0)";
        } else if (p.index == 1) {
            m_vface_used = true;
            base = "float4(vFace)";
        } else {
            return fail("misc-type register " + std::to_string(p.index));
        }
        break;
    case REG_ADDR:
        // Register type 3 is the address register in a vertex shader and the
        // texture coordinate / texture register t# in a pixel shader. SM1
        // writes sample results into t#, so once a texreg is used as a
        // destination the body reads the mutable `tN` rather than the
        // incoming varying.
        if (m_result.stage == ShaderStage::Fragment) {
            m_texcoords_written.insert(p.index);
            if (m_texregs_used.count(p.index) != 0) {
                base = "t" + std::to_string(p.index);
            } else {
                base = "in.texcoord" + std::to_string(p.index);
            }
            break;
        }
        m_address_register_used = true;
        // a0 is stored as int4; promote to float4 for arithmetic sources.
        base = "float4(a0)";
        break;
    case REG_PREDICATE:
        m_predicates_used.insert(p.index);
        base = "p" + std::to_string(p.index);
        break;
    case REG_OUTPUT:
        return fail("reading a vertex shader output register");
    default:
        return fail("source register type " + std::to_string(p.type));
    }

    const std::string swizzle = swizzle_for_mask(p.swizzle, mask);
    std::string expression = base + swizzle;
    const std::string type = vector_type(mask_component_count(mask));

    switch (p.source_modifier) {
    case SRCMOD_NONE:
        break;
    case SRCMOD_NEG:
        expression = "(-" + expression + ")";
        break;
    case SRCMOD_ABS:
        expression = "abs(" + expression + ")";
        break;
    case SRCMOD_ABSNEG:
        expression = "(-abs(" + expression + "))";
        break;
    case SRCMOD_COMP:
        expression = "(" + type + "(1.0) - " + expression + ")";
        break;
    case SRCMOD_X2:
        expression = "(" + expression + " * 2.0)";
        break;
    case SRCMOD_X2NEG:
        expression = "(" + expression + " * -2.0)";
        break;
    case SRCMOD_BIAS:
        expression = "(" + expression + " - " + type + "(0.5))";
        break;
    case SRCMOD_BIASNEG:
        expression = "(" + type + "(0.5) - " + expression + ")";
        break;
    case SRCMOD_SIGN:
        expression = "((" + expression + " - " + type + "(0.5)) * 2.0)";
        break;
    case SRCMOD_SIGNNEG:
        expression = "((" + type + "(0.5) - " + expression + ") * 2.0)";
        break;
    default:
        return fail("source modifier " + std::to_string(p.source_modifier));
    }
    out = expression;
    return true;
}

bool Translator::destination_name(const Parameter& p, std::string& out) {
    if ((p.result_modifier &
         ~(DSTMOD_SATURATE | DSTMOD_PARTIALPRECISION | DSTMOD_CENTROID)) != 0) {
        return fail("destination modifier " +
                    std::to_string(p.result_modifier));
    }
    // Result shift (_x2/_x4/_d2/…) is applied in assign(), not here.

    switch (p.type) {
    case REG_TEMP:
        m_temps_used.insert(p.index);
        out = "r" + std::to_string(p.index);
        return true;
    case REG_RASTOUT:
        // D3D9 rasterizer outs: oPos=0, oFog=1, oPts=2.
        if (p.index == 0) {
            m_position_written = true;
            out = "out.position";
            return true;
        }
        if (p.index == 1) {
            m_result.writes_fog = true;
            out = "out.fog";
            return true;
        }
        if (p.index == 2) {
            m_need_pts_sink = true;
            m_result.writes_point_size = true;
            out = "point_size_value";
            return true;
        }
        return fail("rasterizer output register " + std::to_string(p.index));
    case REG_ATTROUT:
        m_colors_written.insert(p.index);
        out = "out.color" + std::to_string(p.index);
        return true;
    case REG_OUTPUT:
        // SM3 declares o# with a usage; SM2 writes oT# directly. Either way
        // the register index selects a texcoord varying, except where a
        // declaration marked it as the position.
        if (m_result.stage == ShaderStage::Vertex) {
            const auto declared = m_output_declarations.find(p.index);
            if (declared != m_output_declarations.end() &&
                declared->second.usage == Sm3Usage::Position) {
                m_position_written = true;
                out = "out.position";
                return true;
            }
            if (declared != m_output_declarations.end() &&
                declared->second.usage == Sm3Usage::Color) {
                const u32 color = declared->second.usage_index;
                m_colors_written.insert(color);
                out = "out.color" + std::to_string(color);
                return true;
            }
            if (declared != m_output_declarations.end() &&
                declared->second.usage == Sm3Usage::Fog) {
                m_result.writes_fog = true;
                out = "out.fog";
                return true;
            }
            if (declared != m_output_declarations.end() &&
                declared->second.usage == Sm3Usage::PointSize) {
                m_need_pts_sink = true;
                m_result.writes_point_size = true;
                out = "point_size_value";
                return true;
            }
            // A varying is named by its semantic, not by the register that
            // happens to carry it, so the two stages agree even though they
            // are translated independently. Shader model 2 has no output
            // declarations and writes oT#, where the register number is the
            // texture coordinate index.
            const u32 texcoord = declared != m_output_declarations.end()
                                     ? declared->second.usage_index
                                     : p.index;
            m_texcoords_written.insert(texcoord);
            out = "out.texcoord" + std::to_string(texcoord);
            return true;
        }
        return fail("writing an output register from a pixel shader");
    case REG_COLOROUT:
        if (m_result.stage != ShaderStage::Fragment || p.index >= 4) {
            return fail("pixel color output register " +
                        std::to_string(p.index));
        }
        m_outputs_written.insert(p.index);
        out = p.index == 0 ? "out.color"
                           : "out.color" + std::to_string(p.index);
        return true;
    case REG_DEPTHOUT:
        // Learn: oDepth — scalar pixel depth output (ps_2_x / ps_3_0).
        if (m_result.stage != ShaderStage::Fragment) {
            return fail("oDepth outside a pixel shader");
        }
        m_depth_written = true;
        out = "out.depth";
        return true;
    case REG_ADDR:
        // SM1 `tex` / `texcoord` write the texture register t#.
        // Vertex shaders write the address register a0 (mova / mov a0, …).
        if (m_result.stage != ShaderStage::Fragment) {
            if (p.index != 0) {
                return fail("address register a" + std::to_string(p.index));
            }
            m_address_register_used = true;
            out = "a0";
            return true;
        }
        m_texregs_used.insert(p.index);
        m_texcoords_written.insert(p.index);
        out = "t" + std::to_string(p.index);
        return true;
    default:
        return fail("destination register type " + std::to_string(p.type));
    }
}

bool Translator::assign(const Parameter& dst, const std::string& expression) {
    std::string name;
    if (!destination_name(dst, name)) return false;
    const std::string mask = mask_to_string(dst.write_mask);
    const bool saturate = (dst.result_modifier & DSTMOD_SATURATE) != 0;
    // D3DSP_DSTSHIFT is a signed 4-bit power-of-two scale: _x2=1, _d2=-1, …
    std::string scaled = expression;
    if (dst.shift != 0) {
        const int shift =
            static_cast<int>(static_cast<i32>(dst.shift << 28) >> 28);
        scaled = "((" + expression + ") * " +
                 std::to_string(std::ldexp(1.0f, shift)) + ")";
    }
    // a0 is an integer address register, and every legal way to write it
    // converts float to int by rounding to nearest — `mov a0, …` (vs_1_1
    // only) and `mova` (vs_2_0+) alike. Truncating instead reads the
    // neighbouring constant whenever the source lands an ulp below an
    // integer, which is exactly where D3DCOLOR-normalised blend indices
    // (255 * n/255) land in matrix-palette skinning.
    if (dst.type == REG_ADDR && m_result.stage == ShaderStage::Vertex) {
        // Learn (mova - vs): vs_2_x+ a0 is a component vector and any write
        // mask is legal (`mova a0.xz, r0`). The source expression is already
        // swizzled to that width, so the integer constructor must match —
        // `int4(round(float3))` is invalid MSL and Metal rejects the shader.
        const u32 components = mask_component_count(dst.write_mask);
        const std::string as_int =
            int_vector_type(components == 0 ? 4 : components) + "(round(" +
            (saturate ? "saturate(" : "") + scaled +
            (saturate ? ")" : "") + "))";
        m_body << "    " << name << mask << " = " << as_int << ";\n";
        return true;
    }
    if (dst.type == REG_DEPTHOUT) {
        // oDepth is a Metal scalar; never append a .xyzw write mask.
        m_body << "    out.depth = "
               << (saturate ? "saturate(" : "") << scaled
               << (saturate ? ")" : "") << ";\n";
        return true;
    }
    m_body << "    " << name << mask << " = "
           << (saturate ? "saturate(" : "") << scaled
           << (saturate ? ")" : "") << ";\n";
    return true;
}

bool Translator::assign_scalar(const Parameter& dst,
                               const std::string& scalar) {
    const u32 components = mask_component_count(dst.write_mask);
    if (components == 1) return assign(dst, scalar);
    return assign(dst, vector_type(components) + "(" + scalar + ")");
}

bool Translator::int_constant_expression(const Parameter& p,
                                         std::string& out) {
    if (p.type != REG_CONSTINT) {
        return fail("expected an integer constant register");
    }
    if (p.index >= SM3_MAX_INT_CONSTANT_REGISTERS) {
        return fail("integer constant i" + std::to_string(p.index) +
                    " above the D3D9 integer-constant file");
    }
    const auto defined = m_defined_int_constants.find(p.index);
    if (defined != m_defined_int_constants.end()) {
        out = "defi" + std::to_string(p.index);
    } else {
        note_int_constant(p.index);
        out = "cb.i[" + std::to_string(p.index) + "]";
    }
    return true;
}

bool Translator::enclosing_loop_counter() const {
    for (auto it = m_loop_stack.rbegin(); it != m_loop_stack.rend(); ++it) {
        if (it->first == LoopKind::Loop) return true;
    }
    return false;
}

} // namespace vitreo::graphics::sm3_msl_detail
