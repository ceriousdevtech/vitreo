// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include "graphics/shaders/dxbc_to_spirv.hpp"

namespace vitreo::winapi::d3d11 {

/// A positive proof for the original successfully translated DXBC only.
/// Never use reflection, flat binding masks, or optimizer-pruned declarations:
/// raw/structured UAVs, arrays and every register space are writable resources.
[[nodiscard]] inline bool original_dxbc_resources_read_only(
    const graphics::DxbcToSpirvResult& translated) {
    if (!translated.valid() ||
        (translated.stage != graphics::ShaderStage::Vertex &&
         translated.stage != graphics::ShaderStage::Fragment) ||
        !translated.uav_counter_registers.empty() ||
        !translated.uav_counter_spaces.empty() ||
        !translated.capture_registers.empty() ||
        translated.capture_record_vec4s != 0) return false;
    for (const auto& binding : translated.bindings) {
        if (binding.declaration.register_class ==
            graphics::DxbcRegisterClass::UnorderedAccess) return false;
    }
    return true;
}

/// Every fact must be established by the frontend for the FINAL draw route.
/// Unknown/native MSL, generated shader variants and instrumentation stay RW.
struct ReadOnlyIndirectDrawProof {
    bool vertex_shader{false};
    bool pixel_shader{false};
    bool original_shaders{false};
    bool ordinary_stages{false};
    bool no_stream_output{false};
    bool no_statistics{false};
    bool no_injected_writes{false};

    [[nodiscard]] bool valid() const {
        return vertex_shader && pixel_shader && original_shaders &&
               ordinary_stages && no_stream_output && no_statistics &&
               no_injected_writes;
    }
};

/// Whether the MSL actually bound still has the original DXBC resource proof.
/// Vertex-input specialization rewrites only the vertex fetch. It does not
/// add a descriptor-table write, so it does not retire the proof. A generated
/// variant (capture of guest outputs into a new binding, border-colour twins,
/// RGB32 / multisample / minmax rewrites) does.
[[nodiscard]] inline bool descriptor_table_matches_original_proof(
    bool generated_variant, bool /*vertex_input_specialized*/,
    u32 vs_twins, u32 vs_rgb32, u32 vs_ms1, u32 vs_minmax,
    bool pixel_shader_disabled, bool fragment_is_original) {
    return !generated_variant && vs_twins == 0 && vs_rgb32 == 0 &&
           vs_ms1 == 0 && vs_minmax == 0 && !pixel_shader_disabled &&
           fragment_is_original;
}
}  // namespace vitreo::winapi::d3d11
