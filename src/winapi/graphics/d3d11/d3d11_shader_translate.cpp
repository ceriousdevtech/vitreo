// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_shader_translate.cpp
/// @brief DXBC -> SPIR-V -> MSL translation for every D3D11 shader stage,
///        the binding layout a shader needs, and its math contract.
///        Split out of d3d11_pipeline.cpp as verbatim code motion.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_pipeline_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <mach/mach_time.h>

namespace vitreo::winapi {

namespace {

/// Write a shader Vitreo could not translate to a file so it can be replayed
/// offline. Reaching a specific failing shader in a real title otherwise means
/// a full application launch per iteration. Gated on VITREO_D3D11_DIAG, into
/// VITREO_LOG_DIR when that is set. A launched guest may have no usable
/// stdout, so the working directory is not reachable either.
void dump_untranslatable_shader(const u8* bytecode, usize size,
                                const char* stage_tag) {
    const char* diag = std::getenv("VITREO_D3D11_DIAG");
    if (diag == nullptr || diag[0] == '\0' || std::strcmp(diag, "0") == 0) {
        return;
    }
    static std::atomic<unsigned> sequence{0};
    const unsigned index = sequence++;
    const char* dir = std::getenv("VITREO_LOG_DIR");
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/failed_%s_%d_%u.dxbc",
                  (dir != nullptr && dir[0] != '\0') ? dir : ".", stage_tag,
                  static_cast<int>(::getpid()), index);
    if (std::FILE* file = std::fopen(path, "wb")) {
        (void)std::fwrite(bytecode, 1, size, file);
        (void)std::fclose(file);
        pipeline_diag("wrote untranslatable %s shader (%zu bytes) to %s",
                      stage_tag, size, path);
    }
}

/// Highest declared register + 1 across @p bindings, or 0 when none.
[[nodiscard]] u32 register_high_water(
    const std::vector<graphics::DxbcResourceBinding>& bindings) {
    u32 high = 0;
    for (const auto& binding : bindings) {
        const u32 count = std::max(1u, binding.bind_count);
        high = std::max(high, binding.bind_point + count);
    }
    return high;
}

/// VITREO_D3D11_DESCRIPTOR_TABLES: override which binding path a shader takes.
///
/// `force` puts every shader on descriptor tables, `off` keeps every shader on
/// the flat path, and anything else leaves the automatic choice alone.
///
/// This exists because the two paths are hard to compare otherwise. Tables are
/// taken only by shaders that declare registers past the flat capacity, and no
/// oracle scene has one -- the scene shaders are real compiled DXBC and cannot
/// be edited into declaring t8 without breaking their container hash. Forcing
/// the mode runs the whole existing scene corpus down the table path against
/// native Windows, which is coverage no new scene could match. It is also the
/// bisection knob for a production render bug: flip it and see which path owns
/// the defect.
enum class TableOverride { Automatic, Force, Off };

[[nodiscard]] TableOverride table_override() {
    static const TableOverride value = [] {
        const char* setting = std::getenv("VITREO_D3D11_DESCRIPTOR_TABLES");
        if (setting == nullptr) return TableOverride::Automatic;
        if (std::strcmp(setting, "force") == 0) return TableOverride::Force;
        if (std::strcmp(setting, "off") == 0) return TableOverride::Off;
        return TableOverride::Automatic;
    }();
    return value;
}

} // namespace

/// How a shader's registers must be bound, from what it declares.
///
/// A shader whose every register space fits the backend's flat capacity keeps
/// the flat path -- the one that is tested, and that does not pay for a
/// descriptor table it does not need. A shader that declares a register past
/// that capacity cannot be served flat at all: the setter's bounds check drops
/// the binding and the draw renders wrong with no diagnostic. Those shaders read
/// descriptor tables instead.
[[nodiscard]] d3d11::ShaderBindingLayout shader_binding_layout(
    const graphics::DxbcToSpirvResult& translated) {
    d3d11::ShaderBindingLayout layout{};
    layout.srv_slots = register_high_water(translated.texture_bindings);
    layout.constant_buffer_slots =
        register_high_water(translated.cbuffer_bindings);
    layout.sampler_slots = register_high_water(translated.sampler_bindings);
    // The translator refuses more counters than there are bindings.
    for (const u32 reg : translated.uav_counter_registers) {
        layout.uav_counter_registers[layout.uav_counter_count++] =
            static_cast<u8>(reg);
    }
    // D3D10_SB_RESOURCE_DIMENSION (DxbcBindingDeclaration::dimension):
    // 2 texture1d, 3 texture2d, 7 texture1darray, 8 texture2darray -- the
    // shapes the translator declares as 2D or arrayed 2D images.
    for (const auto& resolved : translated.bindings) {
        const auto& decl = resolved.declaration;
        const bool srv =
            decl.register_class == graphics::DxbcRegisterClass::ShaderResource;
        const bool uav =
            decl.register_class == graphics::DxbcRegisterClass::UnorderedAccess;
        const bool flat = decl.dimension == 2u || decl.dimension == 3u;
        const bool arrayed = decl.dimension == 7u || decl.dimension == 8u;
        if ((!srv && !uav) || (!flat && !arrayed) || decl.space != 0) continue;
        const u32 limit = srv ? static_cast<u32>(d3d11::SHADER_RESOURCE_SLOTS)
                              : 64u;
        const u32 upper = std::min(decl.upper, limit - 1u);
        for (u32 reg = decl.lower; reg <= upper; ++reg) {
            const u64 bit = u64{1} << (reg % 64u);
            if (srv) {
                (flat ? layout.srv_flat : layout.srv_arrayed)[reg / 64u] |= bit;
            } else {
                (flat ? layout.uav_flat : layout.uav_arrayed) |= bit;
            }
        }
    }
    const bool past_flat_capacity =
        layout.srv_slots > graphics::BACKEND_MAX_TEXTURE_SLOTS ||
        layout.constant_buffer_slots >
            graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS ||
        layout.sampler_slots > graphics::BACKEND_MAX_TEXTURE_SLOTS;
    switch (table_override()) {
    case TableOverride::Force:
        layout.uses_descriptor_tables = true;
        break;
    case TableOverride::Off:
        // Honest: a shader past the flat capacity still cannot be served, and
        // `off` does not make it servable. It will fail to be created, which is
        // what this frontend did before descriptor tables existed.
        layout.uses_descriptor_tables = false;
        break;
    case TableOverride::Automatic:
        layout.uses_descriptor_tables = past_flat_capacity;
        break;
    }
    return layout;
}

bool D3D11Manager::compile_shader_to_msl(
    const u8* bytecode, usize size, bool is_vertex, std::string& msl_out,
    bool dual_source_blending, d3d11::ShaderBindingLayout* layout_out,
    u32 border_twin_samplers, bool apply_sample_mask, u32 rgb32_buffer_srvs,
    const LogicOpVariant* logic_op, bool count_pixel_invocations,
    u32 single_sample_ms_srvs, bool line_coverage_alpha,
    u32 line_quad_primitive_divisor, u32 minmax_samplers) {
    msl_out.clear();
    if (bytecode == nullptr || size == 0) return false;

    const auto container = graphics::parse_dxbc(bytecode, size);
    if (!container.valid()) {
        pipeline_diag("DXBC parse failed (%s shader, %zu bytes)",
                      is_vertex ? "VS" : "PS", size);
        return false;
    }
    graphics::DxbcTranslationOptions translation_options{};
    translation_options.border_twin_samplers = border_twin_samplers;
    translation_options.minmax_samplers = minmax_samplers;
    translation_options.guard_buffer_bounds = true;
    translation_options.guard_constant_buffers = true;
    translation_options.typed_buffer_bias = true;
    translation_options.fixed_function_push = true;
    translation_options.apply_sample_mask = apply_sample_mask && !is_vertex;
    translation_options.rgb32_buffer_srvs = rgb32_buffer_srvs;
    translation_options.count_pixel_invocations =
        count_pixel_invocations && !is_vertex;
    translation_options.single_sample_ms_srvs = single_sample_ms_srvs;
    translation_options.line_coverage_alpha = line_coverage_alpha && !is_vertex;
    translation_options.line_quad_primitive_divisor =
        is_vertex ? 0u : line_quad_primitive_divisor;
    const bool logic_variant =
        logic_op != nullptr && logic_op->targets != 0 && !is_vertex;
    if (logic_variant) {
        translation_options.logic_op = logic_op->op;
        translation_options.logic_op_targets = logic_op->targets;
        translation_options.logic_op_component_masks =
            logic_op->component_masks;
    }
    auto spirv = graphics::translate_dxbc_to_spirv(container,
                                                   translation_options);
    if (spirv.valid() && border_twin_samplers != 0 &&
        shader_binding_layout(spirv).uses_descriptor_tables) {
        // A descriptor-table shader reads every s# from its sampler table;
        // its border twins follow the sixteen there (sync_descriptor_tables).
        translation_options.border_twin_binding_base = d3d11::SAMPLER_SLOTS;
        spirv = graphics::translate_dxbc_to_spirv(container,
                                                  translation_options);
    }
    if (!spirv.valid()) {
        pipeline_diag("DXBC→SPIR-V failed: %s", spirv.failure.c_str());
        std::fprintf(stderr, "[d3d11] %s DXBC->SPIR-V failed: %s\n",
                     is_vertex ? "VS" : "PS", spirv.failure.c_str());
        dump_untranslatable_shader(bytecode, size, is_vertex ? "vs" : "ps");
        return false;
    }
    const auto stage = is_vertex ? graphics::ShaderStage::Vertex
                                 : graphics::ShaderStage::Fragment;
    const d3d11::ShaderBindingLayout layout = shader_binding_layout(spirv);
    if (layout_out != nullptr) *layout_out = layout;
    if (layout.uses_descriptor_tables &&
        !m_backend.supports_descriptor_tables()) {
        // The shader needs register spaces the flat path cannot reach and the
        // backend cannot serve a table. Fail rather than compile something whose
        // high registers would bind nothing: a shader that renders wrong is
        // harder to diagnose than one that refuses to be created.
        pipeline_diag(
            "%s declares b%u/t%u/s%u, past the flat capacity, and this backend "
            "has no descriptor tables",
            is_vertex ? "VS" : "PS", layout.constant_buffer_slots,
            layout.srv_slots, layout.sampler_slots);
        return false;
    }
    pipeline_diag("%s binding: b%u/t%u/s%u -> %s", is_vertex ? "VS" : "PS",
                  layout.constant_buffer_slots, layout.srv_slots,
                  layout.sampler_slots,
                  layout.uses_descriptor_tables ? "descriptor tables" : "flat");
    graphics::MslCompileOptions msl_options =
        msl_options_for(layout, /*is_compute=*/false);
    raise_msl_version_for_typed_uav_atomics(msl_options, spirv);
    // Exact because the render pipeline compiles this library in Safe mode
    // whenever the shader has a precise instruction (math_mode_for_dxbc).
    msl_options.precise_without_helpers = true;
    msl_options.dual_source_blending = dual_source_blending;
    msl_options.framebuffer_fetch = logic_variant;
    auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), stage, msl_options);
    if (!compiled.valid()) {
        pipeline_diag("SPIR-V→MSL failed for %s", is_vertex ? "VS" : "PS");
        std::fprintf(stderr, "[d3d11] %s SPIR-V->MSL failed\n",
                     is_vertex ? "VS" : "PS");
        dump_untranslatable_shader(bytecode, size, is_vertex ? "vs" : "ps");
        return false;
    }
    msl_out = compiled.msl_text();
    // Opt-in successful-shader capture for diagnosing a draw that the GPU
    // accepts but that produces no fragments. The prefix is host diagnostic
    // state; it never changes guest-visible shader behavior.
    const char* dump_prefix = std::getenv("VITREO_D3D11_DUMP_MSL");
    if (dump_prefix != nullptr && dump_prefix[0] != '\0') {
        static std::atomic<int> vertex_dump_count{0};
        static std::atomic<int> pixel_dump_count{0};
        const int count =
            (is_vertex ? vertex_dump_count : pixel_dump_count).fetch_add(1) + 1;
        if (count <= 64) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s-%d-%s-%d.msl", dump_prefix,
                          static_cast<int>(::getpid()),
                          is_vertex ? "vs" : "ps", count);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(msl_out.data(), 1, msl_out.size(), file);
                std::fclose(file);
                pipeline_diag("wrote MSL dump %s", path);
            }
            std::snprintf(path, sizeof(path), "%s-%d-%s-%d.dxbc", dump_prefix,
                          static_cast<int>(::getpid()),
                          is_vertex ? "vs" : "ps", count);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(bytecode, 1, size, file);
                std::fclose(file);
                pipeline_diag("wrote DXBC dump %s", path);
            }
        }
    }
    return !msl_out.empty();
}

bool D3D11Manager::compile_compute_shader_to_msl(
    const u8* bytecode, usize size, std::string& msl_out,
    u32& workgroup_size_x, u32& workgroup_size_y, u32& workgroup_size_z,
    d3d11::ShaderBindingLayout* layout_out, u32 border_twin_samplers,
    u32 rgb32_buffer_srvs, u32 single_sample_ms_srvs, u32 minmax_samplers) {
    msl_out.clear();
    workgroup_size_x = workgroup_size_y = workgroup_size_z = 0;
    if (bytecode == nullptr || size == 0) return false;

    const auto container = graphics::parse_dxbc(bytecode, size);
    if (!container.valid() || container.stage != graphics::ShaderStage::Compute ||
        !container.is_sm4_plus()) {
        pipeline_diag("DXBC parse/stage validation failed (CS, %zu bytes)",
                      size);
        std::fprintf(stderr,
                     "[d3d11] CS parse/stage failed bytes=%zu\n", size);
        dump_untranslatable_shader(bytecode, size, "cs");
        return false;
    }
    graphics::DxbcTranslationOptions translation_options{};
    translation_options.border_twin_samplers = border_twin_samplers;
    translation_options.guard_buffer_bounds = true;
    translation_options.guard_constant_buffers = true;
    translation_options.typed_buffer_bias = true;
    translation_options.rgb32_buffer_srvs = rgb32_buffer_srvs;
    translation_options.single_sample_ms_srvs = single_sample_ms_srvs;
    translation_options.minmax_samplers = minmax_samplers;
    auto spirv =
        graphics::translate_dxbc_to_spirv(container, translation_options);
    if (spirv.valid() && border_twin_samplers != 0 &&
        shader_binding_layout(spirv).uses_descriptor_tables) {
        // Twins follow the sixteen s# in the sampler table.
        translation_options.border_twin_binding_base = d3d11::SAMPLER_SLOTS;
        spirv = graphics::translate_dxbc_to_spirv(container,
                                                  translation_options);
    }
    if (!spirv.valid() || spirv.stage != graphics::ShaderStage::Compute) {
        pipeline_diag("DXBCâSPIR-V failed for CS: %s",
                      spirv.failure.c_str());
        std::fprintf(stderr, "[d3d11] CS DXBC->SPIR-V failed: %s\n",
                     spirv.failure.c_str());
        dump_untranslatable_shader(bytecode, size, "cs");
        return false;
    }

    const d3d11::ShaderBindingLayout layout = shader_binding_layout(spirv);
    if (layout_out != nullptr) *layout_out = layout;
    if (layout.uses_descriptor_tables &&
        !m_backend.supports_descriptor_tables()) {
        pipeline_diag(
            "CS declares b%u/t%u/s%u, past the flat capacity, and this backend "
            "has no descriptor tables",
            layout.constant_buffer_slots, layout.srv_slots,
            layout.sampler_slots);
        std::fprintf(stderr,
                     "[d3d11] CS descriptor tables b%u/t%u/s%u unsupported\n",
                     layout.constant_buffer_slots, layout.srv_slots,
                     layout.sampler_slots);
        return false;
    }
    pipeline_diag("CS binding: b%u/t%u/s%u -> %s",
                  layout.constant_buffer_slots, layout.srv_slots,
                  layout.sampler_slots,
                  layout.uses_descriptor_tables ? "descriptor tables" : "flat");
    graphics::MslCompileOptions options =
        msl_options_for(layout, /*is_compute=*/true);
    raise_msl_version_for_typed_uav_atomics(options, spirv);
    // Exact because the compute pipeline compiles this library in Safe mode
    // whenever the shader has a precise instruction (math_mode_for_dxbc).
    options.precise_without_helpers = true;
    const auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(),
        graphics::ShaderStage::Compute, options);
    if (!compiled.valid()) {
        pipeline_diag("SPIR-VâMSL failed for CS");
        std::fprintf(stderr, "[d3d11] CS SPIR-V->MSL failed\n");
        dump_untranslatable_shader(bytecode, size, "cs");
        return false;
    }
    msl_out = compiled.msl_text();
    workgroup_size_x = compiled.workgroup_size_x;
    workgroup_size_y = compiled.workgroup_size_y;
    workgroup_size_z = compiled.workgroup_size_z;
    return !msl_out.empty() && workgroup_size_x != 0 &&
           workgroup_size_y != 0 && workgroup_size_z != 0;
}

bool D3D11Manager::compile_geometry_kernel(
    const std::vector<u8>& bytecode, const std::vector<u32>& input_record,
    std::string& msl_out, d3d11::ShaderBindingLayout* layout_out,
    bool tess_software, u32 minmax_samplers) {
    msl_out.clear();
    const auto container =
        graphics::parse_dxbc(bytecode.data(), bytecode.size());
    // A geometry shader's expansion kernel, or (11.17) a hull shader's
    // per-patch kernel: both read the vertex stage's capture records.
    const bool hull = container.valid() &&
                      container.stage == graphics::ShaderStage::Hull;
    if (!container.valid() ||
        (container.stage != graphics::ShaderStage::Geometry && !hull)) {
        return false;
    }
    graphics::DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = !hull;
    options.emulate_tessellation = hull;
    options.domain_to_compute = hull && tess_software;
    options.gs_input_record_registers = input_record;
    // The expansion / hull dispatch binds a push block with the b# limits
    // and the t# typed-buffer biases (fill_typed_buffer_biases).
    options.guard_constant_buffers = true;
    options.typed_buffer_bias = true;
    options.minmax_samplers = minmax_samplers;
    const auto spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (!spirv.valid() ||
        !(hull ? spirv.hs_lowered_to_compute : spirv.gs_lowered_to_compute)) {
        pipeline_diag("%s lowering failed: %s", hull ? "HS" : "GS",
                      spirv.failure.c_str());
        dump_untranslatable_shader(bytecode.data(), bytecode.size(), "gs");
        return false;
    }
    const d3d11::ShaderBindingLayout layout = shader_binding_layout(spirv);
    if (layout_out != nullptr) *layout_out = layout;
    if (layout.uses_descriptor_tables) {
        // The expansion dispatch binds the geometry stage's registers flat;
        // a geometry stage on descriptor tables has no table sync yet.
        pipeline_diag("GS declares b%u/t%u/s%u, past the flat capacity; "
                      "geometry-stage descriptor tables are not wired",
                      layout.constant_buffer_slots, layout.srv_slots,
                      layout.sampler_slots);
        return false;
    }
    graphics::MslCompileOptions msl_options =
        msl_options_for(layout, /*is_compute=*/true);
    raise_msl_version_for_typed_uav_atomics(msl_options, spirv);
    const auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(),
        graphics::ShaderStage::Compute, msl_options);
    if (!compiled.valid()) {
        pipeline_diag("SPIR-V to MSL failed for the GS expansion kernel");
        dump_untranslatable_shader(bytecode.data(), bytecode.size(), "gs");
        return false;
    }
    msl_out = compiled.msl_text();
    return !msl_out.empty();
}

bool D3D11Manager::compile_domain_shader(
    const std::vector<u8>& bytecode, std::string& msl_out,
    d3d11::ShaderBindingLayout* layout_out, std::string& why) {
    msl_out.clear();
    const auto container =
        graphics::parse_dxbc(bytecode.data(), bytecode.size());
    if (!container.valid() ||
        container.stage != graphics::ShaderStage::Domain) {
        why = "not domain-shader bytecode";
        return false;
    }
    graphics::DxbcTranslationOptions options{};
    options.emulate_tessellation = true;
    options.fixed_function_push = true;
    options.guard_constant_buffers = true;
    options.typed_buffer_bias = true;
    const auto spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (!spirv.valid() || !spirv.ds_lowered) {
        why = "domain lowering failed: " + spirv.failure;
        dump_untranslatable_shader(bytecode.data(), bytecode.size(), "ds");
        return false;
    }
    const d3d11::ShaderBindingLayout layout = shader_binding_layout(spirv);
    if (layout_out != nullptr) *layout_out = layout;
    if (layout.uses_descriptor_tables) {
        why = "domain-stage descriptor tables are not wired";
        return false;
    }
    graphics::MslCompileOptions msl_options =
        msl_options_for(layout, /*is_compute=*/false);
    raise_msl_version_for_typed_uav_atomics(msl_options, spirv);
    const auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), graphics::ShaderStage::Domain,
        msl_options);
    if (!compiled.valid()) {
        why = "SPIR-V to MSL failed for the domain shader";
        return false;
    }
    msl_out = compiled.msl_text();
    return !msl_out.empty();
}

bool D3D11Manager::compile_domain_kernel(
    const std::vector<u8>& bytecode, std::string& msl_out,
    d3d11::ShaderBindingLayout* layout_out, std::vector<u32>& records,
    std::string& why, u32 minmax_samplers) {
    msl_out.clear();
    records.clear();
    const auto container =
        graphics::parse_dxbc(bytecode.data(), bytecode.size());
    if (!container.valid() ||
        container.stage != graphics::ShaderStage::Domain) {
        why = "not domain-shader bytecode";
        return false;
    }
    graphics::DxbcTranslationOptions options{};
    options.emulate_tessellation = true;
    options.domain_to_compute = true;
    options.minmax_samplers = minmax_samplers;
    options.typed_buffer_bias = true;
    const auto spirv = graphics::translate_dxbc_to_spirv(container, options);
    if (!spirv.valid() || !spirv.ds_lowered_to_compute) {
        why = "domain lowering to compute failed: " + spirv.failure;
        dump_untranslatable_shader(bytecode.data(), bytecode.size(), "ds");
        return false;
    }
    const d3d11::ShaderBindingLayout layout = shader_binding_layout(spirv);
    if (layout_out != nullptr) *layout_out = layout;
    if (layout.uses_descriptor_tables) {
        why = "domain-stage descriptor tables are not wired";
        return false;
    }
    graphics::MslCompileOptions msl_options =
        msl_options_for(layout, /*is_compute=*/true);
    raise_msl_version_for_typed_uav_atomics(msl_options, spirv);
    const auto compiled = graphics::compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), graphics::ShaderStage::Compute,
        msl_options);
    if (!compiled.valid()) {
        why = "SPIR-V to MSL failed for the domain kernel";
        return false;
    }
    msl_out = compiled.msl_text();
    records = spirv.capture_registers;
    return !msl_out.empty() && !records.empty();
}

/// The Metal math mode a D3D11 shader's float contract requires. D3D11.3
/// 3.1.3.1 honours INF/NaN in every shader; 22.3.2: without
/// refactoringAllowed nothing may be refactored, and 22.19.2: precise
/// components never are. Metal's Relaxed mode allows reassociation but
/// "preserves infs and nans"; Safe allows neither. (Fast, Metal's default,
/// assumes no INF/NaN at all.) Measured over the pixel-shader
/// replay corpus: Safe is the only mode in which 0/0 stays NaN.
[[nodiscard]] graphics::BackendMathMode math_mode_for_dxbc(
    const std::vector<u8>& bytecode) {
    const auto container = graphics::parse_dxbc(bytecode.data(),
                                                bytecode.size());
    if (!container.valid()) return graphics::BackendMathMode::Safe;
    const auto contract = graphics::dxbc_math_contract(container);
    return !contract.refactoring_allowed || contract.uses_precise
               ? graphics::BackendMathMode::Safe
               : graphics::BackendMathMode::Relaxed;
}

[[nodiscard]] bool dxbc_uses_precise(const std::vector<u8>& bytecode) {
    const auto container = graphics::parse_dxbc(bytecode.data(),
                                                bytecode.size());
    return container.valid() &&
           graphics::dxbc_math_contract(container).uses_precise;
}

} // namespace vitreo::winapi
