// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_pipeline_internal.hpp
/// @brief File-local helpers shared by the files split out of
///        d3d11_pipeline.cpp (shaders, state objects, bindings, draw and
///        dispatch). The anonymous namespace holds the stateless helpers:
///        each including TU gets its own internal-linkage copy, as the one
///        TU had before. The declarations after it name the stateful or
///        widely shared helpers, which are defined once. Include from those
///        files only, and never together with d3d11_internal.hpp: the
///        unqualified IID_IUNKNOWN of each collides with the other.

#include "winapi/graphics/d3d11/d3d11.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <vector>

namespace vitreo::winapi {

namespace {

// Not every including file calls every function below; [[maybe_unused]]
// keeps those files free of -Wunused-function without changing codegen.

// Microsoft D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT. The translated fragment
// shader declares one LOD-bias float for every architectural sampler slot,
// including unbound slots whose bias remains zero.
constexpr u32 kD3D11SamplerSlotCount = 16;

using GuidBytes = std::array<u8, 16>;

constexpr GuidBytes IID_IUNKNOWN{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr GuidBytes IID_ID3D11DEVICECHILD{
    0xC8, 0xE5, 0x41, 0x18, 0xB0, 0x16, 0x9B, 0x48,
    0xBC, 0xC8, 0x44, 0xCF, 0xB0, 0xD5, 0xDE, 0xAE};

[[maybe_unused]]
[[nodiscard]] bool guid_eq(const GuidBytes& a, const GuidBytes& b) {
    return a == b;
}

/// The backend view a texture UAV binds through: one mip, and for a
/// Texture1DArray its slice window as a 2D array -- the translator declares a
/// RWTexture1DArray as an arrayed 2D image of height 1.
[[maybe_unused]]
[[nodiscard]] graphics::BackendTextureViewDesc uav_texture_view(
    u32 view_dimension, u32 mip_slice, u32 first_array_slice,
    u32 array_size,
    graphics::PixelFormat view_format = graphics::PixelFormat::Unknown,
    graphics::PixelFormat storage_format = graphics::PixelFormat::Unknown) {
    // D3D11_UAV_DIMENSION_TEXTURE1DARRAY. This file does not include
    // d3d11_internal.hpp, whose IID copies collide with its own.
    constexpr u32 UAV_DIMENSION_TEXTURE1DARRAY = 3;
    constexpr u32 UAV_DIMENSION_TEXTURE2DARRAY = 5;
    graphics::BackendTextureViewDesc desc{};
    const bool array_view =
        view_dimension == UAV_DIMENSION_TEXTURE1DARRAY ||
        view_dimension == UAV_DIMENSION_TEXTURE2DARRAY;
    desc.dimension = array_view
        ? graphics::BackendTextureViewDimension::Texture2DArray
        : graphics::BackendTextureViewDimension::Texture2D;
    desc.most_detailed_mip = mip_slice;
    desc.mip_levels = 1;
    desc.first_array_slice = array_view ? first_array_slice : 0u;
    desc.array_size = array_view ? array_size : 1u;
    // Learn D3D11_UNORDERED_ACCESS_VIEW_DESC.Format: the UAV reads and writes
    // the resource as this format (an R32_UINT view of R32_TYPELESS), not as
    // whatever the storage was created with.
    if (view_format != graphics::PixelFormat::Unknown &&
        view_format != storage_format) {
        desc.pixel_format = view_format;
    }
    return desc;
}

/// MSL options for @p layout. One place, so the shader and the code that binds
/// for it cannot disagree about which mode the shader is in -- a disagreement
/// there is silent, because the shader reads an index nothing was bound at and
/// the draw still succeeds.
// An atomic on a typed UAV is spelled in SPIR-V with OpImageTexelPointer, and
// SPIRV-Cross lowers that to a native Metal texture atomic only from MSL 3.1.
// Below 3.1 it emulates the atomic against a helper buffer it expects the
// caller to bind next to the texture -- which this pipeline does not do, so
// the MSL would still compile and every atomic would land in an unbound
// allocation. Raise the version for exactly those shaders, not globally.
[[maybe_unused]]
void raise_msl_version_for_typed_uav_atomics(
    graphics::MslCompileOptions& options,
    const graphics::DxbcToSpirvResult& spirv) {
    if (spirv.uses_typed_uav_atomics && options.msl_version < 30100u) {
        options.msl_version = 30100u;
    }
}

[[maybe_unused]]
[[nodiscard]] graphics::MslCompileOptions msl_options_for(
    const d3d11::ShaderBindingLayout& layout, bool is_compute) {
    graphics::MslCompileOptions options{};
    if (!layout.uses_descriptor_tables) {
        // Flat: cb# at Metal buffer BACKEND_CONSTANT_BUFFER_BASE + #, textures
        // at texture(#), samplers at sampler(#). Asking SPIRV-Cross for an
        // argument buffer here would leave every resource unresolved, because
        // nothing would encode or bind the table.
        options.argument_buffers = false;
        return options;
    }
    options.argument_buffers = true;
    // The set index IS the D3D register space, as our DXBC translator assigns
    // them: 0 = b#, 1 = t#, 2 = s#, 3 = u#. The buffer index comes from the
    // shared constant the backend binds at, never from a literal here.
    const auto index = [](unsigned set) {
        return graphics::BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
    };
    options.argument_buffer_sets[0] = {layout.constant_buffer_slots, index(0),
                                       graphics::MslDescriptorClass::Buffer};
    options.argument_buffer_sets[1] = {layout.srv_slots, index(1),
                                       graphics::MslDescriptorClass::Texture};
    options.argument_buffer_sets[2] = {layout.sampler_slots, index(2),
                                       graphics::MslDescriptorClass::Sampler};
    // u# becomes a table only where one is actually bound, which today is the
    // compute stage. Declaring an argument buffer for set 3 on a pixel shader
    // that has UAVs would leave the shader reading an unbound buffer -- worse
    // than the flat path's unbound UAV, which reads nothing. Leaving set 3
    // discrete keeps UAV textures on texture indices, a namespace argument
    // buffers do not touch, so the two coexist safely.
    if (is_compute) {
        options.argument_buffer_sets[3] = {
            static_cast<u32>(d3d11::UAV_SLOTS), index(3),
            graphics::MslDescriptorClass::Texture};
    }
    return options;
}

} // namespace

/// d3d11_pipeline_diag.cpp; also declared in d3d11_internal.hpp.
void d3d11_skip_stat(const char* format);
/// d3d11_pipeline_diag.cpp; also declared in d3d11_internal.hpp.
bool d3d11_skip_stats_enabled() noexcept;
/// The per-draw pipeline trace line (d3d11_pipeline_diag.cpp).
void pipeline_diag(const char* format, ...);
/// Whether the pipeline trace is on (d3d11_pipeline_diag.cpp).
[[nodiscard]] bool pipeline_trace_enabled();
/// How a shader's registers must be bound (d3d11_shader_translate.cpp).
[[nodiscard]] d3d11::ShaderBindingLayout shader_binding_layout(
    const graphics::DxbcToSpirvResult& translated);
/// The Metal math mode a shader requires (d3d11_shader_translate.cpp).
[[nodiscard]] graphics::BackendMathMode math_mode_for_dxbc(
    const std::vector<u8>& bytecode);
/// Whether a shader uses precise (d3d11_shader_translate.cpp).
[[nodiscard]] bool dxbc_uses_precise(const std::vector<u8>& bytecode);

} // namespace vitreo::winapi
