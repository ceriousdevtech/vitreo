// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_vitreo_bary.cpp
/// @brief VX_BARY (dxbc_vitreo_ext.hpp): SM 6.1 SV_Barycentrics and
/// GetAttributeAtVertex.
///
/// SV_Barycentrics is SPIR-V BaryCoordKHR / BaryCoordNoPerspKHR
/// (SPV_KHR_fragment_shader_barycentric; MSL [[barycentric_coord]]), with
/// Centroid / Sample for those modes; spirv_to_msl.cpp turns the decoration
/// into MSL's centroid_ / sample_ qualifier, which SPIRV-Cross does not.
///
/// Measured on WARP12 and the RTX 4080 (tests/oracle_data/d3d12_bary_probe*):
/// weight i belongs to vertex i of the triangle in D3D's order (a strip's
/// odd triangle is v1, v3, v2), which is Metal's barycentric_coord order.
/// GetAttributeAtVertex reads flat per-vertex copies (AttributeAtVertex).

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"
#include "graphics/shaders/dxbc_vitreo_ext.hpp"


namespace vitreo::graphics::dxbc_spirv_detail {

using namespace dxbc_vitreo_ext;

bool DxbcTranslator::translate_vx_bary(u32 control) {
    if (m_result.stage != ShaderStage::Fragment) return fail("VX_BARY outside the pixel stage");
    Sm4Operand dst;
    if (!m_decoder.decode_operand(dst)) return fail("VX_BARY: bad destination");
    const BaryOp op = static_cast<BaryOp>(control & 0xFFu);
    if (op == BaryOp::Coord) {
        const u32 mode = (control >> 8) & 0xFu;
        const bool no_perspective = mode == 4u || mode == 5u || mode == 7u;
        u32& variable = no_perspective ? m_vx_bary_no_perspective : m_vx_bary;
        u32& variable_mode = no_perspective ? m_vx_bary_no_perspective_mode : m_vx_bary_mode;
        if (variable != 0 && variable_mode != mode) {
            return fail("VX_BARY: SV_Barycentrics in two interpolation modes of one perspective");
        }
        const bool create = variable == 0;
        if (raster_emulated()) {
            // Metal's barycentric_coord is over pass C's region triangles:
            // the D3D primitive's weights come from pass C itself
            // (kRasterBaryLocation), on the primitive's plane as every other
            // attribute; centroid at the emulation's centroid
            // (D3D11.3 3.5.6.2: the forced pattern's).
            if (create) {
                variable_mode = mode;
                variable = m_builder.emit_variable(m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_vec4),
                                                   SpvStorageClass::Input);
                m_builder.emit_name(variable, no_perspective ? "vitreo_raster_bary_np" : "vitreo_raster_bary");
                m_builder.emit_decorate(variable, SpvDecoration::Location,
                                        no_perspective ? kRasterBaryNoPerspectiveLocation : kRasterBaryLocation);
                if (no_perspective) m_builder.emit_decorate(variable, SpvDecoration::NoPerspective);
                if (mode == 6u || mode == 7u) {
                    m_builder.emit_capability(SpvCapability::SampleRateShading);
                    m_builder.emit_decorate(variable, SpvDecoration::Sample);
                }
                m_vx_interface.push_back(variable);
                m_result.raster_barycentrics |= no_perspective ? 2u : 1u;
            }
            u32 weights = 0;
            if (mode == 3u || mode == 5u) {
                m_builder.emit_capability(SpvCapability::InterpolationFunction);
                weights = m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450, kGlslStd450InterpolateAtOffset,
                                                  {variable, m_builder.emit_load(m_type_vec2, m_re_centroid_offset)});
            } else {
                weights = m_builder.emit_load(m_type_vec4, variable);
            }
            const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
            const u32 result = m_builder.emit_composite_construct(
                m_type_vec4, {m_builder.emit_composite_extract(m_type_float, weights, {0}),
                              m_builder.emit_composite_extract(m_type_float, weights, {1}),
                              m_options.line_barycentrics ? zero
                                                          : m_builder.emit_composite_extract(m_type_float, weights, {2}),
                              zero});
            return store_vec4(dst, result);
        }
        m_builder.emit_extension("SPV_KHR_fragment_shader_barycentric");
        m_builder.emit_capability(SpvCapability::FragmentBarycentricKHR);
        const u32 weights = vx_builtin(no_perspective ? SpvBuiltIn::BaryCoordNoPerspKHR : SpvBuiltIn::BaryCoordKHR,
                                       m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_vec3), m_type_vec3,
                                       variable, no_perspective ? "vitreo_bary_noperspective" : "vitreo_bary");
        if (create) {
            variable_mode = mode;
            if (mode == 3u || mode == 5u) m_builder.emit_decorate(variable, SpvDecoration::Centroid);
            if (mode == 6u || mode == 7u) {
                m_builder.emit_capability(SpvCapability::SampleRateShading);
                m_builder.emit_decorate(variable, SpvDecoration::Sample);
            }
        }
        const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
        const u32 result = m_builder.emit_composite_construct(
            m_type_vec4, {m_builder.emit_composite_extract(m_type_float, weights, {0}),
                          m_builder.emit_composite_extract(m_type_float, weights, {1}),
                          m_options.line_barycentrics ? zero
                                                      : m_builder.emit_composite_extract(m_type_float, weights, {2}),
                          zero});
        return store_vec4(dst, result);
    }
    if (op == BaryOp::AttributeAtVertex) {
        // GetAttributeAtVertex: Metal 4's vertex_value<T> needs Apple10
        // ("Access to pre-raster per-vertex values", Metal Feature Set
        // Tables), so the triangle's three values arrive as flat inputs a
        // pass-through vertex stage writes from the captured vertices
        // (gs_passthrough.hpp per_vertex_registers).
        Sm4Operand v;
        Sm4Operand k;
        if (!m_decoder.decode_operand(v) || !m_decoder.decode_operand(k)) {
            return fail("VX_BARY AttributeAtVertex: bad operands");
        }
        if (v.type != Sm4OperandType::Input || v.index0_relative || v.index0 >= 32u) {
            return fail("VX_BARY AttributeAtVertex: not a v# register");
        }
        const auto value_type = m_input_value_types.find(v.index0);
        if (value_type == m_input_value_types.end()) {
            return fail("VX_BARY AttributeAtVertex: v" + std::to_string(v.index0) + " is not declared");
        }
        auto& vars = m_vx_per_vertex[v.index0];
        if (vars[0] == 0) {
            const u32 pointer = m_builder.emit_type_pointer(SpvStorageClass::Input, value_type->second);
            for (u32 i = 0; i < 3; ++i) {
                vars[i] = m_builder.emit_variable(pointer, SpvStorageClass::Input);
                m_builder.emit_name(vars[i], "vitreo_per_vertex" + std::to_string(v.index0) + "_" + std::to_string(i));
                m_builder.emit_decorate(vars[i], SpvDecoration::Location, per_vertex_input_location(v.index0, i));
                m_builder.emit_decorate(vars[i], SpvDecoration::Flat);
                m_vx_interface.push_back(vars[i]);
            }
            m_result.per_vertex_inputs |= 1u << v.index0;
        }
        std::array<u32, 3> values{};
        for (u32 i = 0; i < 3; ++i) {
            values[i] = m_builder.emit_load(value_type->second, vars[i]);
            if (value_type->second != m_type_vec4) values[i] = m_builder.emit_bitcast(m_type_vec4, values[i]);
        }
        u32 index = 0;
        if (!load_vec4(k, index)) return false;
        index = m_builder.emit_composite_extract(m_type_uint, m_builder.emit_bitcast(m_type_uvec4, index), {0});
        // 0, 1 or 2 (DXIL.rst AttributeAtVertex: VertexID); anything else
        // reads vertex 0.
        const auto is = [&](u32 n) {
            const u32 b = m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, index,
                                                         m_builder.emit_constant_int(m_type_uint, n));
            return m_builder.emit_composite_construct(m_type_bvec4, {b, b, b, b});
        };
        u32 result = m_builder.emit_select(m_type_vec4, is(2), values[2], values[0]);
        result = m_builder.emit_select(m_type_vec4, is(1), values[1], result);
        return store_vec4(dst, apply_swizzle(result, v.swizzle));
    }
    return fail("VX_BARY: unknown operation");
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
