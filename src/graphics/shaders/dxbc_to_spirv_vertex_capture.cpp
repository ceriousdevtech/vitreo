// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_vertex_capture.cpp
/// @brief Pass-A vertex capture.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

// Pass A declarations: the buffer the capture writes, and the vertex index it
// keys records by.
//
// The record layout is the one the geometry pass reads --
// `record[vertex * register_count + i]`, i being the index of that output
// register in ascending register order -- so the two sides sort identically.
bool DxbcTranslator::emit_vertex_capture_declarations() {
    for (const auto& [reg, _] : m_output_vars) {
        m_capture_registers.push_back(reg);
    }
    if (m_capture_registers.empty()) {
        return fail("vertex capture: the shader declares no outputs to "
                    "capture");
    }
    // Writable here, unlike the geometry pass's view of the same binding:
    // pass A produces these records and pass B consumes them.
    m_capture_buffer = declare_expansion_buffer(
        m_type_vec4, 16u, "vitreo_gs_in", 0u, /*read_only=*/false);
    if (m_vertex_id_input != 0) {
        // Already in the interface through m_input_scalar_builtins.
        m_capture_vertex_index = m_vertex_id_input;
    } else {
        m_capture_vertex_index = m_builder.emit_variable(
            m_type_ptr_input_uint, SpvStorageClass::Input);
        m_builder.emit_name(m_capture_vertex_index,
                            "vitreo_capture_vertex_index");
        m_builder.emit_decorate(m_capture_vertex_index, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::VertexIndex));
        m_builtin_inputs[static_cast<u32>(SpvBuiltIn::VertexIndex)] =
            m_capture_vertex_index;
    }
    if (m_options.capture_draw_relative) {
        // Keys relative to the draw: SV_VertexID's value (VertexIndex -
        // BaseVertex, D3D11.3 8.16) plus the instance's stride.
        m_builder.emit_capability(SpvCapability::DrawParameters);
        m_builder.emit_extension("SPV_KHR_shader_draw_parameters");
        (void)builtin_input(SpvBuiltIn::BaseVertex, "vitreo_base_vertex");
        (void)builtin_input(SpvBuiltIn::InstanceIndex,
                            "vitreo_capture_instance");
        (void)builtin_input(SpvBuiltIn::BaseInstance, "vitreo_base_instance");
    }
    m_result.captures_vertex_outputs = true;
    m_result.capture_record_vec4s =
        static_cast<u32>(m_capture_registers.size());
    m_result.capture_registers = m_capture_registers;
    return true;
}

u32 DxbcTranslator::builtin_input(SpvBuiltIn builtin, const char* name) {
    const auto found = m_builtin_inputs.find(static_cast<u32>(builtin));
    if (found != m_builtin_inputs.end()) return found->second;
    const u32 var =
        m_builder.emit_variable(m_type_ptr_input_uint, SpvStorageClass::Input);
    m_builder.emit_name(var, name);
    m_builder.emit_decorate(var, SpvDecoration::BuiltIn,
                            static_cast<u32>(builtin));
    m_builtin_inputs[static_cast<u32>(builtin)] = var;
    m_capture_extra_inputs.push_back(var);
    return var;
}

// Pass A epilogue: copy every output register into the capture buffer. Runs
// once, at the end of the function, which is why an early return is refused
// rather than tolerated -- a vertex that returned early would leave a stale
// or zero record and the geometry pass would expand garbage for it.
bool DxbcTranslator::emit_vertex_capture_epilogue() {
    if (m_emitted_early_return) {
        return fail("vertex capture: the shader returns early, so the capture "
                    "would not run for every vertex");
    }
    const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
    const u32 vec4_ptr = m_builder.emit_type_pointer(
        SpvStorageClass::StorageBuffer, m_type_vec4);
    u32 vertex_index =
        m_builder.emit_load(m_type_uint, m_capture_vertex_index);
    const u32 record_count = m_builder.emit_constant_int(
        m_type_uint, static_cast<u32>(m_capture_registers.size()));
    u32 record_base = 0;
    u32 capture_done = 0;
    if (m_options.capture_draw_relative) {
        // The buffer opens with a 16-byte header the GPU resolve of the
        // indirect draw wrote -- {keys per instance, record capacity} -- and
        // the records follow it. A key outside either is not written.
        const u32 local = m_builder.emit_isub(
            m_type_uint, vertex_index,
            m_builder.emit_load(
                m_type_uint,
                builtin_input(SpvBuiltIn::BaseVertex, "vitreo_base_vertex")));
        const u32 instance = m_builder.emit_isub(
            m_type_uint,
            m_builder.emit_load(
                m_type_uint, builtin_input(SpvBuiltIn::InstanceIndex,
                                           "vitreo_capture_instance")),
            m_builder.emit_load(
                m_type_uint, builtin_input(SpvBuiltIn::BaseInstance,
                                           "vitreo_base_instance")));
        const u32 header = m_builder.emit_bitcast(
            m_type_uvec4,
            m_builder.emit_load(
                m_type_vec4,
                m_builder.emit_access_chain(vec4_ptr, m_capture_buffer,
                                            {member, member})));
        const u32 stride =
            m_builder.emit_composite_extract(m_type_uint, header, {0u});
        const u32 capacity =
            m_builder.emit_composite_extract(m_type_uint, header, {1u});
        const u32 key = m_builder.emit_iadd(
            m_type_uint, local,
            m_builder.emit_imul(m_type_uint, instance, stride));
        const u32 inside = m_builder.emit_logical(
            SpvOp::OpLogicalAnd, m_type_bool,
            m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool,
                                           local, stride),
            m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool,
                                           key, capacity));
        const u32 write_block = m_builder.allocate_id();
        capture_done = m_builder.allocate_id();
        m_builder.emit_selection_merge(capture_done);
        m_builder.emit_branch_conditional(inside, write_block, capture_done);
        m_builder.emit_label_id(write_block);
        record_base = m_builder.emit_iadd(
            m_type_uint, m_builder.emit_imul(m_type_uint, key, record_count),
            m_builder.emit_constant_int(m_type_uint, 1u));
        vertex_index = key;
    } else {
        record_base =
            m_builder.emit_imul(m_type_uint, vertex_index, record_count);
    }
    for (usize i = 0; i < m_capture_registers.size(); ++i) {
        const u32 reg = m_capture_registers[i];
        const auto var = m_output_vars.find(reg);
        const auto value_type = m_output_value_types.find(reg);
        if (var == m_output_vars.end() ||
            value_type == m_output_value_types.end()) {
            return fail("vertex capture: output o" + std::to_string(reg) +
                        " was declared but has no variable");
        }
        const u32 loaded = m_builder.emit_load(value_type->second, var->second);
        // The buffer is untyped storage: an integer output is reinterpreted,
        // never converted, exactly as the geometry side reads it back.
        const u32 raw = value_type->second == m_type_vec4
                            ? loaded
                            : m_builder.emit_bitcast(m_type_vec4, loaded);
        const u32 slot = m_builder.emit_iadd(
            m_type_uint, record_base,
            m_builder.emit_constant_int(m_type_uint, static_cast<u32>(i)));
        m_builder.emit_store(
            m_builder.emit_access_chain(vec4_ptr, m_capture_buffer,
                                        {member, slot}),
            raw);
    }
    if (capture_done != 0) {
        m_builder.emit_branch(capture_done);
        m_builder.emit_label_id(capture_done);
    }
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
