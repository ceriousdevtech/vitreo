// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv.cpp
/// @brief Direct3D 11 shader model 4–5 bytecode to SPIR-V translator.
///
/// SM4/SM5 instruction encoding (SHDR/SHEX chunk):
/// - Each instruction is 1+ DWORD tokens
/// - Token 0: [10:0]=opcode, [30:24]=length in DWORDs (incl. opcode),
///   [31]=extended-opcode flag (Microsoft D3D10 Tokenized Program Format)
/// - Operands follow, each operand is 1+ tokens:
///   - [11:0]=index, [12]=index repr, [19:13]=operand type, [21:20]=index dim,
///     [23:22]=num components, [31:24]=selection mode/swizzle
///
/// Reference:
/// - Microsoft D3D11 Shader Bytecode Reference (public docs)
/// - SPIR-V 1.6 specification

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics {

/// Iterations one loop may run per invocation before its guard breaks out.
u32 shader_loop_iteration_cap() {
    static const u32 cap = [] {
        if (const char* text = std::getenv("VITREO_SHADER_LOOP_CAP")) {
            return static_cast<u32>(std::strtoul(text, nullptr, 10));
        }
        return u32{1} << 14;
    }();
    return cap;
}

namespace dxbc_spirv_detail {

DxbcToSpirvResult DxbcTranslator::run() {
    m_result.stage = m_container.stage;
    m_result.shader_model_major = m_container.shader_model_major;
    m_result.shader_model_minor = m_container.shader_model_minor;

    if (!scan_geometry_declarations()) return m_result;
    if (!scan_compute_declarations()) return m_result;
    if (!scan_tessellation_declarations()) return m_result;
    if (!scan_class_linkage()) return m_result;
    if (hs_as_compute()) {
        // From here the module is a compute kernel: one thread per patch,
        // every phase a function it calls. See
        // .docs/design-tessellation-emulation.md.
        m_result.hs_lowered_to_compute = true;
        m_result.stage = ShaderStage::Compute;
        m_compute_group_size_x = 1u;
        m_compute_group_size_y = 1u;
        m_compute_group_size_z = 1u;
    }
    m_result.ds_lowered = ds_lowered();
    if (ds_as_compute()) {
        // One thread per domain point the software tessellator generated
        // (.docs/design-tessellation-emulation.md, exact path).
        m_result.ds_lowered_to_compute = true;
        m_result.stage = ShaderStage::Compute;
        m_compute_group_size_x = kDomainComputeThreads;
        m_compute_group_size_y = 1u;
        m_compute_group_size_z = 1u;
    }
    // The host cannot size the expansion buffer or choose a primitive type
    // without these, so they are reported for every geometry shader whether
    // or not it is being lowered.
    m_result.gs_input_primitive = m_gs_input_primitive;
    m_result.gs_output_topology = m_gs_output_topology;
    m_result.gs_max_output_vertices = m_gs_max_output_vertices;
    m_result.gs_instance_count = m_gs_instance_count;
    m_result.gs_stream_mask = m_gs_stream_mask;
    if (gs_as_compute()) {
        // Refuse the shapes the expansion does not cover, here rather than
        // part-way through emission, so a rejected shader never leaves a
        // half-built module behind.
        if (m_gs_input_primitive != 1u && m_gs_input_primitive != 2u &&
            m_gs_input_primitive != 3u && m_gs_input_primitive != 4u &&
            m_gs_input_primitive != 6u) {
            (void)fail("geometry emulation: an input primitive of " +
                       std::to_string(m_gs_input_primitive) +
                       " vertices is not a point, line, triangle or their "
                       "adjacency forms");
            return m_result;
        }
        if (m_gs_output_topology != 1u && m_gs_output_topology != 3u &&
            m_gs_output_topology != 5u) {
            (void)fail("geometry emulation: output topology " +
                       std::to_string(m_gs_output_topology) +
                       " is not pointlist, linestrip or trianglestrip");
            return m_result;
        }
        // From here the module really is a compute shader: GLCompute entry
        // point, one thread per input primitive, and the expansion buffers in
        // descriptor set 4. Every site that would otherwise emit a
        // geometry-stage construct asks gs_as_compute() instead.
        m_result.gs_lowered_to_compute = true;
        m_result.stage = ShaderStage::Compute;
        m_compute_group_size_x = 1u;  // one primitive per thread
        m_compute_group_size_y = 1u;
        m_compute_group_size_z = 1u;
    }
    m_result.workgroup_size_x = m_compute_group_size_x;
    m_result.workgroup_size_y = m_compute_group_size_y;
    m_result.workgroup_size_z = m_compute_group_size_z;

    if (!emit_header()) return m_result;
    if (!emit_types()) return m_result;
    if (!emit_variables()) return m_result;
    if (!emit_function()) return m_result;
    {
        const std::set<u32> listed(m_entry_interface.begin(),
                                   m_entry_interface.end());
        for (const u32 var : m_builder.private_variables()) {
            if (!listed.contains(var)) m_entry_interface.push_back(var);
        }
    }
    m_builder.emit_entry_point(m_entry_model, m_main_func, "main",
                               m_entry_interface);

    m_result.spirv = m_builder.finalize(m_builder.allocate_id());
    m_result.ok = true;
    return m_result;
}

bool DxbcTranslator::emit_header() {
    // Capabilities
    m_builder.emit_capability(SpvCapability::Shader);
    if (m_result.stage == ShaderStage::Geometry) {
        m_builder.emit_capability(SpvCapability::Geometry);
    }
    if (ds_lowered()) m_builder.emit_capability(SpvCapability::Tessellation);
    // Declared eagerly because the translator predeclares typed MS-array
    // image variants; the capability is inert for shaders that do not use one.
    m_builder.emit_capability(SpvCapability::ImageMSArray);
    m_builder.emit_capability(SpvCapability::ImageQuery);
    m_glsl_std450 = m_builder.emit_ext_inst_import("GLSL.std.450");

    // Memory model
    m_builder.emit_memory_model();

    return true;
}

} // namespace dxbc_spirv_detail

// =============================================================================
// Public API
// =============================================================================

DxbcMathContract dxbc_math_contract(const DxbcContainer& container) {
    return dxbc_spirv_detail::math_contract_of(container);
}

DxbcToSpirvResult translate_dxbc_to_spirv(
    const DxbcContainer& container,
    const DxbcTranslationOptions& options) {
    if (!container.valid()) {
        DxbcToSpirvResult result{};
        result.ok = false;
        result.failure = "Invalid DXBC container";
        return result;
    }

    if (!container.is_sm4_plus()) {
        DxbcToSpirvResult result{};
        result.ok = false;
        result.failure = "Shader model must be 4.0 or higher for DXBC translation";
        return result;
    }

    dxbc_spirv_detail::DxbcTranslator translator(container, options);
    return translator.run();
}

} // namespace vitreo::graphics
