// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_pipeline_objects.cpp
/// @brief Creation and release of D3D11 shader, input-layout and state
///        objects (and the buffer-fill helpers kept beside them).
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

/// Diagnostic survey mode: keep a shader object alive even though its
/// translation failed, so one run enumerates *every* shader the guest cannot
/// yet run instead of stopping at the first. UE treats a failed CreateShader
/// as fatal, which otherwise makes each missing opcode cost a full launch.
/// Never on by default: a surveyed shader has no MSL and cannot draw.
[[nodiscard]] bool shader_survey_mode() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D11_SHADER_SURVEY");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

} // namespace

D3D11Manager::ShaderTranslation D3D11Manager::translate_shader(
    const u8* bytecode, usize size, graphics::ShaderStage stage) {
    ShaderTranslation result{};
    if (stage == graphics::ShaderStage::Compute) {
        result.translated = compile_compute_shader_to_msl(
            bytecode, size, result.msl, result.workgroup_size_x,
            result.workgroup_size_y, result.workgroup_size_z, &result.layout);
    } else {
        result.translated = compile_shader_to_msl(
            bytecode, size, stage == graphics::ShaderStage::Vertex,
            result.msl, false, &result.layout);
    }
    return result;
}

GuestAddr D3D11Manager::create_vertex_shader(
    GuestAddr device_owner, const void* bytecode, usize bytecode_length,
    bool hidden, ShaderTranslation* translation) {
    if (bytecode == nullptr || bytecode_length == 0 || !ensure_storage()) {
        return 0;
    }
    ShaderTranslation local{};
    if (translation == nullptr) {
        local = translate_shader(static_cast<const u8*>(bytecode),
                                 bytecode_length, graphics::ShaderStage::Vertex);
        translation = &local;
    }
    if (!translation->translated && !shader_survey_mode()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_vertex_shader_vtable);
    if (address == 0) return 0;
    VertexShader shader{};
    shader.interface_address = address;
    shader.device_owner = device_owner;
    shader.bytecode.assign(static_cast<const u8*>(bytecode),
                           static_cast<const u8*>(bytecode) + bytecode_length);
    shader.msl_source = std::move(translation->msl);
    shader.binding_layout = translation->layout;
    const auto container =
        graphics::parse_dxbc(static_cast<const u8*>(bytecode), bytecode_length);
    if (container.valid()) {
        for (const auto& input : container.input_signature) {
            ShaderSignatureElement el{};
            el.semantic_name = input.semantic_name;
            el.semantic_index = input.semantic_index;
            el.register_num = input.register_num;
            el.component_type = input.component_type;
            shader.input_signature.push_back(std::move(el));
        }
        for (const auto& output : container.output_signature) {
            if (output.system_value_type == 5u) {  // D3D_NAME_VIEWPORT_ARRAY_INDEX
                shader.writes_viewport_index = true;
            }
        }
    }
    if (hidden) shader.public_refs = 0;
    // Start the backend compile now, on the creating thread (UE's loaders),
    // with the key the first draw will ask for (execute_draw: entry main0,
    // the shader's math mode and precise flag) instead of on the RHI thread
    // when that draw arrives.
    if (!shader.msl_source.empty()) {
        m_backend.prefetch_shader(shader.msl_source, "main0",
                                  math_mode_for_dxbc(shader.bytecode),
                                  dxbc_uses_precise(shader.bytecode));
    }
    m_vertex_shaders.emplace(address, std::move(shader));
    // The application's creation reference holds the device.
    if (!hidden) acquire_parent(device_owner);
    return address;
}

GuestAddr D3D11Manager::create_pixel_shader(
    GuestAddr device_owner, const void* bytecode, usize bytecode_length,
    bool hidden, ShaderTranslation* translation) {
    if (bytecode == nullptr || bytecode_length == 0 || !ensure_storage()) {
        return 0;
    }
    ShaderTranslation local{};
    if (translation == nullptr) {
        local = translate_shader(static_cast<const u8*>(bytecode),
                                 bytecode_length,
                                 graphics::ShaderStage::Fragment);
        translation = &local;
    }
    if (!translation->translated && !shader_survey_mode()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_pixel_shader_vtable);
    if (address == 0) return 0;
    PixelShader shader{};
    shader.interface_address = address;
    shader.device_owner = device_owner;
    shader.bytecode.assign(static_cast<const u8*>(bytecode),
                           static_cast<const u8*>(bytecode) + bytecode_length);
    shader.msl_source = std::move(translation->msl);
    shader.binding_layout = translation->layout;
    const auto container =
        graphics::parse_dxbc(static_cast<const u8*>(bytecode), bytecode_length);
    if (container.valid()) {
        shader.early_depth_uav_writes =
            graphics::dxbc_forces_early_depth_stencil(container) &&
            graphics::dxbc_writes_memory(container) &&
            !graphics::dxbc_pixel_runs_per_sample(container);
        for (const auto& output : container.output_signature) {
            // oMask is named, not numbered (dxbc_to_spirv.cpp, SV_Coverage);
            // semantic spelling is case-insensitive.
            const std::string& name = output.semantic_name;
            const auto named = [&](std::string_view lower) {
                return name.size() == lower.size() &&
                       std::equal(name.begin(), name.end(), lower.begin(),
                                  [](char a, char b) {
                                      return std::tolower(
                                                 static_cast<unsigned char>(
                                                     a)) == b;
                                  });
            };
            if (named("sv_coverage")) shader.writes_coverage = true;
            if (named("sv_depth") || named("sv_depthgreaterequal") ||
                named("sv_depthlessequal")) {
                shader.writes_depth = true;
            }
        }
    }
    if (hidden) shader.public_refs = 0;
    // As create_vertex_shader: the fragment key of the first draw is main0,
    // the shader's math mode, no invariance.
    if (!shader.msl_source.empty()) {
        m_backend.prefetch_shader(shader.msl_source, "main0",
                                  math_mode_for_dxbc(shader.bytecode), false);
    }
    m_pixel_shaders.emplace(address, std::move(shader));
    // The application's creation reference holds the device.
    if (!hidden) acquire_parent(device_owner);
    return address;
}

GuestAddr D3D11Manager::create_compute_shader(
    GuestAddr device_owner, const void* bytecode, usize bytecode_length,
    bool hidden, ShaderTranslation* translation) {
    if (bytecode == nullptr || bytecode_length == 0 || !ensure_storage()) {
        return 0;
    }
    ShaderTranslation local{};
    if (translation == nullptr) {
        local = translate_shader(static_cast<const u8*>(bytecode),
                                 bytecode_length,
                                 graphics::ShaderStage::Compute);
        translation = &local;
    }
    if (!translation->translated && !shader_survey_mode()) {
        std::fprintf(stderr,
                     "[d3d11] CreateComputeShader translation failed "
                     "bytes=%zu\n",
                     bytecode_length);
        return 0;
    }
    const GuestAddr address = allocate_object(m_compute_shader_vtable);
    if (address == 0) return 0;
    ComputeShader shader{};
    shader.interface_address = address;
    shader.device_owner = device_owner;
    shader.bytecode.assign(static_cast<const u8*>(bytecode),
                           static_cast<const u8*>(bytecode) + bytecode_length);
    shader.msl_source = std::move(translation->msl);
    shader.binding_layout = translation->layout;
    shader.workgroup_size_x = translation->workgroup_size_x;
    shader.workgroup_size_y = translation->workgroup_size_y;
    shader.workgroup_size_z = translation->workgroup_size_z;
    if (hidden) shader.public_refs = 0;
    m_compute_shaders.emplace(address, std::move(shader));
    // The application's creation reference holds the device.
    if (!hidden) acquire_parent(device_owner);
    return address;
}

void D3D11Manager::replicate_element(u8* dst, usize span,
                                     std::span<const u8> element) noexcept {
    const usize size = element.size();
    if (dst == nullptr || size == 0 || span == 0) return;
    if (std::all_of(element.begin() + 1, element.end(),
                    [&](u8 b) { return b == element[0]; })) {
        std::memset(dst, element[0], span);
        return;
    }
    // Each pass doubles a prefix that is a whole number of elements.
    usize filled = std::min(size, span);
    std::memcpy(dst, element.data(), filled);
    while (filled < span) {
        const usize chunk = std::min(filled, span - filled);
        std::memcpy(dst + filled, dst, chunk);
        filled += chunk;
    }
}

bool D3D11Manager::fill_buffer_on_gpu(Buffer& buffer, usize begin,
                                      std::span<const u8> element,
                                      usize span) {
    if (!buffer.backend || element.empty() || span == 0 ||
        (span % element.size()) != 0) {
        return false;
    }
    const bool one_byte =
        std::all_of(element.begin() + 1, element.end(),
                    [&](u8 b) { return b == element[0]; });
    if (one_byte && m_backend.fill_buffer_region(buffer.backend.get(), begin,
                                                 span, element[0])) {
        buffer.gpu_newer = true;
        note_gpu_use(buffer);
        return true;
    }
    std::vector<u8> pattern(span);
    replicate_element(pattern.data(), span, element);
    graphics::BackendBufferDesc desc{};
    desc.byte_width = span;
    auto source = m_backend.create_buffer(desc, pattern.data());
    if (!source || !source->valid()) return false;
    // The recorded copy retains the source's Metal buffer until it executes,
    // so the transient wrapper can go out of scope here.
    if (!m_backend.copy_buffer_region(buffer.backend.get(), begin, source.get(),
                                      0, span)) {
        return false;
    }
    buffer.gpu_newer = true;
    note_gpu_use(buffer);
    return true;
}

GuestAddr D3D11Manager::create_input_layout(
    GuestAddr device_owner,
    const std::vector<InputLayoutElement>& elements) {
    if (!ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_input_layout_vtable);
    if (address == 0) return 0;
    InputLayout layout{};
    layout.interface_address = address;
    layout.device_owner = device_owner;
    layout.elements = elements;
    // Map the IA slots this layout reads onto the backend's streams. A slot
    // below the stream count keeps its own stream, so a layout that only uses
    // slots 0-7 binds exactly as before; a higher slot takes the lowest stream
    // no low slot of this layout claims. Stream 0 is never lent out: it is also
    // the legacy single-stream binding. A slot that finds no free stream stays
    // kNoBackendStream, and a draw with it fails loudly.
    layout.backend_stream.fill(InputLayout::kNoBackendStream);
    layout.stream_slot.fill(InputLayout::kNoBackendStream);
    for (const auto& element : elements) {
        const u32 slot = element.input_slot;
        if (slot < graphics::BACKEND_MAX_VERTEX_BUFFERS) {
            layout.backend_stream[slot] = static_cast<u8>(slot);
            layout.stream_slot[slot] = static_cast<u8>(slot);
        }
    }
    for (const auto& element : elements) {
        const u32 slot = element.input_slot;
        if (slot < graphics::BACKEND_MAX_VERTEX_BUFFERS ||
            slot >= layout.backend_stream.size() ||
            layout.backend_stream[slot] != InputLayout::kNoBackendStream) {
            continue;
        }
        for (u32 stream = 1; stream < graphics::BACKEND_MAX_VERTEX_BUFFERS;
             ++stream) {
            if (layout.stream_slot[stream] == InputLayout::kNoBackendStream) {
                layout.stream_slot[stream] = static_cast<u8>(slot);
                layout.backend_stream[slot] = static_cast<u8>(stream);
                break;
            }
        }
    }
    m_input_layouts.emplace(address, std::move(layout));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    return address;
}

GuestAddr D3D11Manager::create_blend_state(
    GuestAddr device_owner, const BlendStateDesc& desc) {
    const GuestAddr address = allocate_object(m_blend_state_vtable);
    if (address == 0) return 0;
    BlendState state{};
    state.interface_address = address;
    state.device_owner = device_owner;
    state.desc = desc;
    m_blend_states.emplace(address, std::move(state));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    return address;
}

GuestAddr D3D11Manager::create_rasterizer_state(
    GuestAddr device_owner, const RasterizerStateDesc& desc) {
    const GuestAddr address = allocate_object(m_rasterizer_state_vtable);
    if (address == 0) return 0;
    RasterizerState state{};
    state.interface_address = address;
    state.device_owner = device_owner;
    state.desc = desc;
    m_rasterizer_states.emplace(address, std::move(state));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    return address;
}

GuestAddr D3D11Manager::create_depth_stencil_state(
    GuestAddr device_owner, const DepthStencilStateDesc& desc) {
    const GuestAddr address = allocate_object(m_depth_stencil_state_vtable);
    if (address == 0) return 0;
    DepthStencilState state{};
    state.interface_address = address;
    state.device_owner = device_owner;
    state.desc = desc;
    m_depth_stencil_states.emplace(address, std::move(state));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    return address;
}

GuestAddr D3D11Manager::create_sampler_state(
    GuestAddr device_owner, const SamplerStateDesc& desc) {
    const GuestAddr address = allocate_object(m_sampler_state_vtable);
    if (address == 0) return 0;
    SamplerState state{};
    state.interface_address = address;
    state.device_owner = device_owner;
    state.desc = desc;
    m_sampler_states.emplace(address, std::move(state));
    ++m_sampler_set_generation;
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    return address;
}

u64 D3D11Manager::msl_hash(const std::string& msl, bool shader_owned) {
    if (!shader_owned) return std::hash<std::string>{}(msl);
    const auto [found, inserted] = m_msl_hashes.try_emplace(&msl, 0);
    if (inserted) found->second = std::hash<std::string>{}(msl);
    return found->second;
}

u32 D3D11Manager::release_vertex_shader(GuestAddr address) {
    auto it = m_vertex_shaders.find(address);
    if (it == m_vertex_shaders.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        ShaderLinkage linkage = std::move(it->second.linkage);
        m_vertex_shaders.erase(it);
        forget_msl_hashes();
        free_object(address);
        destroy_specialisations(LinkedStage::Vertex, linkage);
    }
    return count;
}

u32 D3D11Manager::release_pixel_shader(GuestAddr address) {
    auto it = m_pixel_shaders.find(address);
    if (it == m_pixel_shaders.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        ShaderLinkage linkage = std::move(it->second.linkage);
        m_pixel_shaders.erase(it);
        forget_msl_hashes();
        free_object(address);
        destroy_specialisations(LinkedStage::Pixel, linkage);
    }
    return count;
}

u32 D3D11Manager::release_compute_shader(GuestAddr address) {
    auto it = m_compute_shaders.find(address);
    if (it == m_compute_shaders.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        ShaderLinkage linkage = std::move(it->second.linkage);
        m_compute_shaders.erase(it);
        free_object(address);
        destroy_specialisations(LinkedStage::Compute, linkage);
    }
    return count;
}

u32 D3D11Manager::release_input_layout(GuestAddr address) {
    auto it = m_input_layouts.find(address);
    if (it == m_input_layouts.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0) {
        m_input_layouts.erase(it);
        free_object(address);
    }
    return count;
}

// A state object's final Release removes it from the deduplication pool, so
// the same description then makes a fresh object without the old private data
// (measured, d3d11_a04_state_objects_probe "blend after-last-release
// recreate"). The runtime's own default object stays (StatePoolEntry).
u32 D3D11Manager::release_blend_state(GuestAddr address) {
    auto it = m_blend_states.find(address);
    if (it == m_blend_states.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0 && !it->second.pool.runtime_default) {
        state_pool_erase(STATE_FAMILY_BLEND, it->second.device_owner,
                         it->second.pool.pool_key);
        m_blend_states.erase(it);
        free_object(address);
    }
    return count;
}

u32 D3D11Manager::release_rasterizer_state(GuestAddr address) {
    auto it = m_rasterizer_states.find(address);
    if (it == m_rasterizer_states.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0 && !it->second.pool.runtime_default) {
        state_pool_erase(STATE_FAMILY_RASTERIZER, it->second.device_owner,
                         it->second.pool.pool_key);
        m_rasterizer_states.erase(it);
        free_object(address);
    }
    return count;
}

u32 D3D11Manager::release_depth_stencil_state(GuestAddr address) {
    auto it = m_depth_stencil_states.find(address);
    if (it == m_depth_stencil_states.end() || it->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --it->second.ref_count;
    if (count == 0 && !it->second.pool.runtime_default) {
        state_pool_erase(STATE_FAMILY_DEPTH_STENCIL, it->second.device_owner,
                         it->second.pool.pool_key);
        m_depth_stencil_states.erase(it);
        free_object(address);
    }
    return count;
}

u32 D3D11Manager::release_sampler_state(GuestAddr address) {
    auto it = m_sampler_states.find(address);
    if (it == m_sampler_states.end() || it->second.ref_count == 0) return 0;
    const u32 count = --it->second.ref_count;
    if (count == 0 && !it->second.pool.runtime_default) {
        state_pool_erase(STATE_FAMILY_SAMPLER, it->second.device_owner,
                         it->second.pool.pool_key);
        m_sampler_states.erase(it);
        ++m_sampler_set_generation;
        forget_sampler_memo(address);
        free_object(address);
    }
    return count;
}

} // namespace vitreo::winapi
