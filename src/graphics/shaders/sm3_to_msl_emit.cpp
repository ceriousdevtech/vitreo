// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_emit.cpp
/// @brief Assembles the translated SM1-3 body into a Metal shader.

#include "sm3_to_msl_internal.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <sstream>
#include <string>

namespace vitreo::graphics::sm3_msl_detail {

namespace {

/// A def value as an MSL literal that reads back as exactly the same float:
/// nine significant digits round-trip any IEEE single (the stream default
/// of six turned 1/127 into 0.00787402).
std::string float_literal(float value) {
    if (std::isnan(value)) return "NAN";
    if (std::isinf(value)) return value > 0 ? "INFINITY" : "(-INFINITY)";
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    std::string literal = text;
    if (literal.find_first_of(".en") == std::string::npos) literal += ".0";
    return literal;
}

} // namespace


std::string Translator::build_msl() const {
    std::ostringstream ss;
    ss << "#include <metal_stdlib>\n"
       << "using namespace metal;\n\n";

    // Declare exactly the registers the shader reads. A fixed 256-entry file
    // would be 4 KiB, which is the whole inline-constant budget of a draw.
    // Layout must match d3d9_draw.cpp packing: float4 c[], then int4 i[],
    // then int b[] (0/1), with the CPU buffer padded to 16 bytes.
    ss << "struct Sm3Constants {\n"
       << "    float4 c[" << std::max(m_result.float_constant_count, 1u)
       << "];\n";
    if (m_result.int_constant_count > 0) {
        ss << "    int4 i[" << m_result.int_constant_count << "];\n";
    }
    if (m_result.bool_constant_count > 0) {
        ss << "    int b[" << m_result.bool_constant_count << "];\n";
    }
    if (m_result.uses_bump_env) {
        ss << "    float4 bump_env[16];\n";
    }
    if (m_result.stage == ShaderStage::Vertex && m_result.needs_viewport) {
        // Packed after the 16-byte-padded c/i/b file. See d3d9_draw.cpp.
        ss << "    float4 viewport;\n";
    }
    ss << "};\n\n";

    if (m_relative_const_used) {
        // Learn: Address Register — reads outside the legal constant range
        // return (0,0,0,0). Clamping the *result* to the last slot would
        // sample a bone matrix / light the shader never addressed. The
        // subscript itself is still clamped: Metal may evaluate both sides
        // of a ternary, and an OOB `cb.c[i]` aborts the GPU.
        // Learn (def - vs/ps): a constant the shader defines replaces that
        // register for the whole shader, relative reads included -- a
        // c[a0.x + n] that lands on a def'd register returns the def.
        // Full-render measurements justify LUTs for large DEF sets. Keep the
        // switch through 95 definitions to avoid small-shader source/startup bloat.
        // Only vertex shaders permit relative reads of the constant file.
        const bool def_lut = m_result.stage == ShaderStage::Vertex &&
                             m_defined_constants.size() >= 96u;
        if (def_lut) {
            std::array<u32, (SM3_MAX_FLOAT_CONSTANT_REGISTERS + 31u) / 32u> present{};
            ss << "constant uint4 sm3_def_values[" << SM3_MAX_FLOAT_CONSTANT_REGISTERS << "] = {\n";
            for (u32 i = 0; i < SM3_MAX_FLOAT_CONSTANT_REGISTERS; ++i) {
                std::array<u32, 4> bits{};
                const auto value = m_defined_constants.find(i);
                if (value != m_defined_constants.end()) {
                    present[i / 32u] |= 1u << (i % 32u);
                    for (u32 lane = 0; lane < 4; ++lane)
                        std::memcpy(&bits[lane], &value->second[lane], sizeof(u32));
                }
                ss << "    uint4(" << bits[0] << "u, " << bits[1] << "u, "
                   << bits[2] << "u, " << bits[3] << "u),\n";
            }
            ss << "};\nconstant uint sm3_def_present[" << present.size() << "] = {";
            for (u32 word : present) ss << word << "u,";
            ss << "};\n\n";
        }
        ss << "float4 sm3_c(constant Sm3Constants& cb, int i) {\n";
        if (!def_lut && !m_defined_constants.empty()) {
            ss << "    switch (i) {\n";
            for (const auto& [index, value] : m_defined_constants) {
                ss << "    case " << index << ": return float4("
                   << float_literal(value[0]) << ", " << float_literal(value[1]) << ", "
                   << float_literal(value[2]) << ", " << float_literal(value[3]) << ");\n";
            }
            ss << "    default: break;\n    }\n";
        }
        if (def_lut) {
            ss << "    const int n = " << SM3_MAX_FLOAT_CONSTANT_REGISTERS << ";\n"
               << "    if (i < 0 || i >= n) return float4(0.0);\n"
               << "    const uint idx = uint(max(min(i, n - 1), 0));\n"
               << "    const bool defined = (sm3_def_present[idx >> 5u] & (1u << (idx & 31u))) != 0u;\n"
               << "    return defined ? as_type<float4>(sm3_def_values[idx]) : cb.c[idx];\n"
               << "}\n\n";
        } else {
        ss << "    const int n = "
           << std::max(m_result.float_constant_count, 1u) << ";\n"
           << "    const int idx = max(min(i, n - 1), 0);\n"
           << "    const float4 v = cb.c[idx];\n"
           << "    return (i < 0 || i >= n) ? float4(0.0) : v;\n"
           << "}\n\n";
        }

        // Check the signed offset before adding the nonnegative literal base.
        // This avoids C++/MSL signed overflow for INT_MIN/INT_MAX offsets.
        ss << "float4 sm3_c(constant Sm3Constants& cb, int offset, int base) {\n"
           << "    if (offset < -base || offset >= " << SM3_MAX_FLOAT_CONSTANT_REGISTERS
           << " - base) return float4(0.0);\n"
           << "    return sm3_c(cb, offset + base);\n}\n\n";
    }

    const bool vertex = m_result.stage == ShaderStage::Vertex;

    if (vertex) {
        ss << "struct Sm3VertexIn {\n";
        for (const Sm3InputElement& input : m_result.inputs) {
            ss << "    float4 v" << input.register_index << " [[attribute("
               << input.register_index << ")]];\n";
        }
        ss << "};\n\n";
    }

    // The varying set is shared between the two stages by name, so a vertex
    // and a pixel shader compiled separately still line up in one pipeline.
    ss << "struct Sm3Varyings {\n";
    if (vertex) ss << "    float4 position [[position]];\n";
    else        ss << "    float4 frag_position [[position]];\n";
    for (u32 color : m_colors_written) {
        ss << "    float4 color" << color << " [[user(color" << color
           << ")]];\n";
    }
    for (u32 texcoord : m_texcoords_written) {
        ss << "    float4 texcoord" << texcoord << " [[user(texcoord"
           << texcoord << ")]];\n";
    }
    if (vertex && m_result.writes_fog) {
        ss << "    float4 fog [[user(fog)]];\n";
    }
    ss << "};\n\n";

    if (!vertex) {
        ss << "struct Sm3FragmentOut {\n"
           << "    float4 color [[color(0)]];\n";
        for (u32 output : m_outputs_written) {
            if (output != 0) {
                ss << "    float4 color" << output << " [[color(" << output
                   << ")]];\n";
            }
        }
        if (m_depth_written) {
            ss << "    float depth [[depth(any)]];\n";
        }
        ss << "};\n\n";
    }

    const auto emit_defined_constants = [&](std::ostringstream& out) {
        for (const auto& [index, value] : m_defined_constants) {
            out << "    const float4 def" << index << " = float4("
                << float_literal(value[0]) << ", " << float_literal(value[1]) << ", "
                << float_literal(value[2]) << ", " << float_literal(value[3])
                << ");\n";
        }
        for (const auto& [index, value] : m_defined_int_constants) {
            out << "    const int4 defi" << index << " = int4(" << value[0]
                << ", " << value[1] << ", " << value[2] << ", " << value[3]
                << ");\n";
        }
        for (const auto& [index, value] : m_defined_bool_constants) {
            out << "    const bool defb" << index << " = "
                << (value ? "true" : "false") << ";\n";
        }
    };

    // Subroutines become Metal functions so call/ret map to ordinary calls.
    // Immediate def/defi/defb are re-declared inside each function.
    if (m_uses_subroutines) {
        const std::string params = subroutine_param_list();
        for (const auto& [label, body] : m_label_bodies) {
            ss << "void sm3_l" << label << "(" << params << ")\n{\n";
            emit_defined_constants(ss);
            ss << expand_calls(body);
            ss << "}\n\n";
        }
    }

    const auto emit_sampler_parameters = [&] {
        for (u32 sampler = 0; sampler < m_result.samplers_used.size();
             ++sampler) {
            if (!m_result.samplers_used[sampler]) continue;
            // ps_1_x declares no samplers: a stage takes the dimension of
            // the texture bound to it.
            const u8 dim = m_result.major == 1 ? m_stage_dim[sampler]
                                               : m_sampler_dim[sampler];
            const char* texture_ty = "texture2d";
            if (dim == 3) {
                texture_ty = "texturecube";
            } else if (dim == 4) {
                texture_ty = "texture3d";
            }
            ss << ",\n    " << texture_ty << "<float> tex" << sampler
               << " [[texture(" << sampler << ")]]"
               << ",\n    sampler samp" << sampler << " [[sampler(" << sampler
               << ")]]";
        }
    };

    if (vertex) {
        ss << "vertex Sm3Varyings " << m_result.entry_point << "(\n"
           << "    Sm3VertexIn in [[stage_in]],\n"
           << "    constant Sm3Constants& cb [[buffer(0)]]";
        emit_sampler_parameters();
        ss << ")\n{\n"
           << "    Sm3Varyings out;\n";
    } else {
        ss << "fragment Sm3FragmentOut " << m_result.entry_point << "(\n"
           << "    Sm3Varyings in [[stage_in]],\n"
           << "    constant Sm3Constants& cb [[buffer(0)]]";
        if (m_vface_used) {
            ss << ",\n    bool front_facing [[front_facing]]";
        }
        emit_sampler_parameters();
        ss << ")\n{\n"
           << "    Sm3FragmentOut out;\n";
        if (m_vface_used) {
            // Learn: sign(vFace) < 0 ⇒ back face.
            ss << "    const float vFace = front_facing ? 1.0 : -1.0;\n";
        }
    }

    // Zero-initialize so a component the shader never writes reads as zero,
    // which is what a D3D9 register holds before its first write.
    for (u32 temp : m_temps_used) {
        ss << "    float4 r" << temp << " = float4(0.0);\n";
    }
    if (m_address_register_used) {
        ss << "    int4 a0 = int4(0);\n";
    }
    if (m_uses_subroutines) {
        // Shared with subroutines; loop blocks may shadow with a local aL.
        ss << "    int aL = 0;\n";
    }
    if (m_need_pts_sink) {
        ss << "    float4 point_size_value = float4(0.0);\n";
    }
    for (u32 texreg : m_texregs_used) {
        ss << "    float4 t" << texreg << " = in.texcoord" << texreg
           << ";\n";
    }
    for (u32 predicate : m_predicates_used) {
        ss << "    bool4 p" << predicate << " = bool4(false);\n";
    }
    emit_defined_constants(ss);
    if (vertex) {
        ss << "    out.position = float4(0.0, 0.0, 0.0, 1.0);\n";
        for (u32 color : m_colors_written) {
            ss << "    out.color" << color << " = float4(0.0);\n";
        }
        for (u32 texcoord : m_texcoords_written) {
            // Learn Texture Coordinate Register: write .xy for 2D; projection
            // needs all four. Unwritten .w must be 1 so a .xy-only write is
            // the identity divide, not Inf UVs (torn textures / giant tris).
            ss << "    out.texcoord" << texcoord
               << " = float4(0.0, 0.0, 0.0, 1.0);\n";
        }
        if (m_result.writes_fog) {
            ss << "    out.fog = float4(0.0);\n";
        }
    } else {
        ss << "    out.color = float4(0.0);\n";
        for (u32 output : m_outputs_written) {
            if (output != 0) {
                ss << "    out.color" << output << " = float4(0.0);\n";
            }
        }
    }

    // run() flushes the active stream into m_main_body before build_msl.
    if (m_uses_subroutines) {
        ss << expand_calls(m_main_body);
    } else {
        ss << m_main_body;
    }
    if (vertex && m_result.needs_viewport) {
        // D3D9 POSITIONT is already in screen pixels (pixel centres) with
        // RHW in .w. Metal [[position]] is clip space. Same map as the
        // fixed-function XYZRHW path in ff_emulation.cpp.
        ss << "    {\n"
           << "        const float rhw = (out.position.w != 0.0) ? "
              "out.position.w : 1.0;\n"
           << "        const float clip_w = 1.0 / rhw;\n"
           << "        const float2 screen = out.position.xy + "
              "float2(0.5, 0.5);\n"
           << "        const float2 ndc = float2(\n"
           << "            screen.x / max(cb.viewport.x, 1.0) * 2.0 - 1.0,\n"
           << "            1.0 - screen.y / max(cb.viewport.y, 1.0) * 2.0);\n"
           << "        out.position = float4(ndc * clip_w, "
              "out.position.z * clip_w, clip_w);\n"
           << "    }\n";
    }
    ss << "    return out;\n}\n";
    return ss.str();
}

} // namespace vitreo::graphics::sm3_msl_detail
