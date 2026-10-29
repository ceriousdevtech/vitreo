// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_declarations.cpp
/// @brief SM1-3 dcl, def, defi and defb.

#include "sm3_to_msl_internal.hpp"

#include <array>
#include <cstring>
#include <string>

namespace vitreo::graphics::sm3_msl_detail {

bool Translator::handle_declaration(u32 control) {
    if (!has(2)) return fail("a truncated declaration");
    const u32 usage_token = take();
    const Parameter dst = decode_destination(take());

    if (dst.type == REG_SAMPLER) {
        if (dst.index >= m_result.samplers_used.size()) {
            return fail("a sampler register above s15");
        }
        // Bits 27:24 of the usage token are D3DSAMPLER_TEXTURE_TYPE
        // (Microsoft Learn dcl_samplerType): unknown/2d/cube/volume.
        constexpr u32 kD3Dstt2D = 2;
        constexpr u32 kD3DsttCube = 3;
        constexpr u32 kD3DsttVolume = 4;
        const u32 texture_type = (usage_token >> 27) & 0xF;
        if (texture_type != 0 && texture_type != kD3Dstt2D &&
            texture_type != kD3DsttCube && texture_type != kD3DsttVolume) {
            return fail("a non-2D sampler declaration (type=" +
                        std::to_string(texture_type) + ")");
        }
        m_result.samplers_used[dst.index] = true;
        m_sampler_dim[dst.index] =
            static_cast<u8>(texture_type == 0 ? kD3Dstt2D : texture_type);
        return true;
    }

    const auto usage = static_cast<Sm3Usage>(usage_token & 0x1F);
    const u32 usage_index = (usage_token >> 16) & 0xF;
    (void)control;

    if (m_result.stage == ShaderStage::Vertex) {
        if (dst.type == REG_INPUT) {
            m_result.inputs.push_back({usage, usage_index, dst.index});
            if (usage == Sm3Usage::PositionT) {
                m_result.needs_viewport = true;
            }
            return true;
        }
        if (dst.type == REG_OUTPUT) {
            m_output_declarations.emplace(
                dst.index, Sm3InputElement{usage, usage_index, dst.index});
            return true;
        }
        return fail("a vertex shader declaration on register type " +
                    std::to_string(dst.type));
    }

    if (dst.type == REG_INPUT) {
        // Learn: dcl - ps. ps_2_0 declares colour with `dcl v#` — v0 is
        // COLOR0 and v1 is COLOR1. The leading token in the stream is not a
        // D3DDECLUSAGE (that form is ps_3_0); using it as usage_index collapses
        // every v# onto color0 and bleaches UI that adds v1.a into RGB.
        if (m_result.major < 3) {
            m_input_declarations.emplace(
                dst.index,
                Sm3InputElement{Sm3Usage::Color, dst.index, dst.index});
            return true;
        }
        m_input_declarations.emplace(
            dst.index, Sm3InputElement{usage, usage_index, dst.index});
        return true;
    }
    if (dst.type == REG_ADDR) {
        // SM2 `dcl t#` — texture-coordinate inputs use the texture register
        // file (Learn: dcl - ps). Reading t# already maps to in.texcoordN.
        m_texcoords_written.insert(dst.index);
        return true;
    }
    if (dst.type == REG_OUTPUT || dst.type == REG_MISCTYPE) {
        return true;
    }
    return fail("a pixel shader declaration on register type " +
                std::to_string(dst.type));
}

bool Translator::handle_define() {
    if (!has(5)) return fail("a truncated constant definition");
    const Parameter dst = decode_destination(take());
    std::array<float, 4> value{};
    for (usize i = 0; i < 4; ++i) {
        const u32 bits = take();
        std::memcpy(&value[i], &bits, sizeof(float));
    }
    if (dst.type != REG_CONST) {
        return fail("a constant definition on register type " +
                    std::to_string(dst.type));
    }
    m_defined_constants.emplace(dst.index, value);
    return true;
}

bool Translator::handle_define_int() {
    // Learn: defi - vs/ps. Four signed 32-bit immediates; take precedence
    // over Set*ShaderConstantI when the shader is bound.
    if (!has(5)) return fail("a truncated integer constant definition");
    const Parameter dst = decode_destination(take());
    std::array<i32, 4> value{};
    for (usize i = 0; i < 4; ++i) {
        value[i] = static_cast<i32>(take());
    }
    if (dst.type != REG_CONSTINT) {
        return fail("an integer constant definition on register type " +
                    std::to_string(dst.type));
    }
    if (dst.index >= SM3_MAX_INT_CONSTANT_REGISTERS) {
        return fail("integer constant i" + std::to_string(dst.index) +
                    " above the D3D9 integer-constant file");
    }
    m_defined_int_constants.emplace(dst.index, value);
    return true;
}

bool Translator::handle_define_bool() {
    // Learn: defb - vs/ps. One boolean immediate; takes precedence over
    // Set*ShaderConstantB when the shader is bound.
    if (!has(2)) return fail("a truncated boolean constant definition");
    const Parameter dst = decode_destination(take());
    const u32 raw = take();
    if (dst.type != REG_CONSTBOOL) {
        return fail("a boolean constant definition on register type " +
                    std::to_string(dst.type));
    }
    if (dst.index >= SM3_MAX_BOOL_CONSTANT_REGISTERS) {
        return fail("boolean constant b" + std::to_string(dst.index) +
                    " above the D3D9 boolean-constant file");
    }
    m_defined_bool_constants.emplace(dst.index, raw != 0);
    return true;
}

} // namespace vitreo::graphics::sm3_msl_detail
