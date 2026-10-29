// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_object_storage.cpp
/// @brief The guest storage arena the D3D11 interfaces live in: the vtable
///        layout and its compile-time disjointness check, ensure_storage, and
///        the object arena's growth, allocation and free list. Split out of
///        d3d11.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

// Where each vtable sits inside the manager's storage region, and how many
// slots it holds. A vtable written past the next one's offset overwrites that
// neighbour's first slots with its own thunks: the guest then calls, say,
// ID3D11Fence::QueryInterface and lands in a query method. Raising a count
// (as the `1` interfaces did) must therefore move the table, and this check
// says so at compile time instead of at the first guest call. Slots are sized
// for a 64-bit guest, the larger pointer.
struct VtableSpan {
    usize offset;
    usize slots;
};
constexpr usize kVtableDevice = 0x0, kVtableContext = 0x300,
                kVtableDxgiDevice = 0xA00, kVtableBuffer = 0xB00,
                kVtableTexture2d = 0xC00, kVtableDxgiResource = 0xC80,
                kVtableShaderResourceView = 0xD00,
                kVtableRenderTargetView = 0xE00,
                kVtableDepthStencilView = 0xF00, kVtableQuery = 0x1000,
                kVtableFence = 0x1080,
                kVtableAnnotation = 0x1200,
                kVtableVertexShader = 0x1300, kVtablePixelShader = 0x1380,
                kVtableInputLayout = 0x1400, kVtableBlendState = 0x1480,
                kVtableRasterizerState = 0x1500,
                kVtableDepthStencilState = 0x1580,
                kVtableSamplerState = 0x1600,
                kVtableComputeShader = 0x1700, kVtableKeyedMutex = 0x1780,
                kVtableUnorderedAccessView = 0x1800,
                kVtableGeometryShader = 0x1880, kVtableDxgiSurface = 0x1900,
                kVtableDeviceContextState = 0x1980,
                kVtableHullShader = 0x1A00, kVtableDomainShader = 0x1A80,
                // The command list used to sit where a 64-bit table ran into
                // its neighbour (0x10E0, 8 slots, into the swap chain at
                // 0x1100), as did a video-device table at 0x1680 (20 slots,
                // into the compute shader at 0x1700). Whichever was written
                // second replaced the other's slots.
                kVtableCommandList = 0x1B00,
                // IDXGISwapChain4 is 41 slots (0x148 bytes), which no longer
                // fits the 0x1100..0x1200 gap the IDXGISwapChain1 table had.
                kVtableSwapChain = 0x1C80,
                kVtableSwapChainMedia = 0x1E00,
                kVtableMultithread = 0x1E80, kVtableClassLinkage = 0x1F00,
                kVtableClassInstance = 0x1F80,
                // The D3D10 facets (winapi/graphics/d3d10/d3d10_device.cpp).
                kVtableD3D10Device = 0x2000, kVtableD3D10Buffer = 0x2340,
                kVtableD3D10Texture = 0x23C0,
                kVtableD3D10ShaderResourceView = 0x2440,
                kVtableD3D10RenderTargetView = 0x24C0,
                kVtableD3D10DepthStencilView = 0x2540,
                kVtableD3D10BlendState = 0x25C0,
                kVtableD3D10DepthStencilState = 0x2640,
                kVtableD3D10RasterizerState = 0x2680,
                kVtableD3D10SamplerState = 0x26C0,
                kVtableD3D10DeviceChild = 0x2700, kVtableD3D10Query = 0x2740,
                kVtableD3D10Blob = 0x27C0, kVtableD3D10StateBlock = 0x2800,
                // ID3D11VideoContext3: 85 slots (0x2A8 bytes).
                kVtableVideoContext = 0x2C00,
                kVtableRegionEnd = 0x3000;
constexpr std::array<VtableSpan, 48> kVtableLayout{{
    {kVtableD3D10Device, d3d10::DEVICE_METHOD_COUNT},
    {kVtableD3D10Buffer, d3d10::BUFFER_METHOD_COUNT},
    {kVtableD3D10Texture, d3d10::TEXTURE_METHOD_COUNT},
    {kVtableD3D10ShaderResourceView,
     d3d10::SHADER_RESOURCE_VIEW_METHOD_COUNT},
    {kVtableD3D10RenderTargetView, d3d10::RENDER_TARGET_VIEW_METHOD_COUNT},
    {kVtableD3D10DepthStencilView, d3d10::DEPTH_STENCIL_VIEW_METHOD_COUNT},
    {kVtableD3D10BlendState, d3d10::BLEND_STATE_METHOD_COUNT},
    {kVtableD3D10DepthStencilState, d3d10::DEPTH_STENCIL_STATE_METHOD_COUNT},
    {kVtableD3D10RasterizerState, d3d10::RASTERIZER_STATE_METHOD_COUNT},
    {kVtableD3D10SamplerState, d3d10::SAMPLER_STATE_METHOD_COUNT},
    {kVtableD3D10DeviceChild, d3d10::DEVICE_CHILD_METHOD_COUNT},
    {kVtableD3D10Query, d3d10::QUERY_METHOD_COUNT},
    {kVtableD3D10Blob, d3d10::BLOB_METHOD_COUNT},
    {kVtableD3D10StateBlock, d3d10::STATE_BLOCK_METHOD_COUNT},
    {kVtableMultithread, d3d11::MULTITHREAD_METHOD_COUNT},
    {kVtableClassLinkage, d3d11::CLASS_LINKAGE_METHOD_COUNT},
    {kVtableClassInstance, d3d11::CLASS_INSTANCE_METHOD_COUNT},
    {kVtableSwapChainMedia, d3d11::SWAP_CHAIN_MEDIA_METHOD_COUNT},
    {kVtableDevice, d3d11::DEVICE_METHOD_COUNT},
    {kVtableContext, d3d11::CONTEXT_METHOD_COUNT},
    {kVtableDxgiDevice, d3d11::DXGI_DEVICE_METHOD_COUNT},
    {kVtableBuffer, d3d11::BUFFER_METHOD_COUNT},
    {kVtableTexture2d, d3d11::TEXTURE_2D_METHOD_COUNT},
    {kVtableDxgiResource, d3d11::DXGI_RESOURCE_METHOD_COUNT},
    {kVtableShaderResourceView, d3d11::SHADER_RESOURCE_VIEW_METHOD_COUNT},
    {kVtableRenderTargetView, d3d11::RENDER_TARGET_VIEW_METHOD_COUNT},
    {kVtableDepthStencilView, d3d11::DEPTH_STENCIL_VIEW_METHOD_COUNT},
    {kVtableQuery, d3d11::QUERY_METHOD_COUNT},
    {kVtableFence, d3d11::FENCE_METHOD_COUNT},
    {kVtableCommandList, d3d11::COMMAND_LIST_METHOD_COUNT},
    {kVtableSwapChain, d3d11::SWAP_CHAIN_METHOD_COUNT},
    {kVtableAnnotation, d3d11::ANNOTATION_METHOD_COUNT},
    {kVtableVertexShader, d3d11::VERTEX_SHADER_METHOD_COUNT},
    {kVtablePixelShader, d3d11::PIXEL_SHADER_METHOD_COUNT},
    {kVtableInputLayout, d3d11::INPUT_LAYOUT_METHOD_COUNT},
    {kVtableBlendState, d3d11::BLEND_STATE_METHOD_COUNT},
    {kVtableRasterizerState, d3d11::RASTERIZER_STATE_METHOD_COUNT},
    {kVtableDepthStencilState, d3d11::DEPTH_STENCIL_STATE_METHOD_COUNT},
    {kVtableSamplerState, d3d11::SAMPLER_STATE_METHOD_COUNT},
    {kVtableVideoContext, d3d11::VIDEO_CONTEXT_METHOD_COUNT},
    {kVtableComputeShader, d3d11::COMPUTE_SHADER_METHOD_COUNT},
    {kVtableKeyedMutex, d3d11::KEYED_MUTEX_METHOD_COUNT},
    {kVtableUnorderedAccessView, d3d11::UNORDERED_ACCESS_VIEW_METHOD_COUNT},
    {kVtableGeometryShader, d3d11::GEOMETRY_SHADER_METHOD_COUNT},
    {kVtableDxgiSurface, d3d11::DXGI_SURFACE_METHOD_COUNT},
    {kVtableDeviceContextState, d3d11::DEVICE_CONTEXT_STATE_METHOD_COUNT},
    {kVtableHullShader, d3d11::HULL_SHADER_METHOD_COUNT},
    {kVtableDomainShader, d3d11::DOMAIN_SHADER_METHOD_COUNT},
}};
consteval bool vtable_layout_is_disjoint() {
    for (usize i = 0; i < kVtableLayout.size(); ++i) {
        const usize end_i = kVtableLayout[i].offset + kVtableLayout[i].slots * 8;
        if (end_i > kVtableRegionEnd) return false;
        for (usize j = 0; j < kVtableLayout.size(); ++j) {
            if (i == j) continue;
            const usize begin_j = kVtableLayout[j].offset;
            const usize end_j = begin_j + kVtableLayout[j].slots * 8;
            if (kVtableLayout[i].offset < end_j && begin_j < end_i) {
                return false;
            }
        }
    }
    return true;
}
static_assert(vtable_layout_is_disjoint(),
              "a D3D11 vtable overlaps its neighbour or the object region");

} // namespace

bool D3D11Manager::ensure_storage() {
    if (m_storage_ready) return true;
    if (m_region_base != 0) return false;

    // Prefer the gap at 0x63000000; on x86 retry lower if needed.
    for (GuestAddr minimum = 0x63000000ull; minimum != 0;) {
        const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0) {
            if (m_is_64bit || minimum != 0x63000000ull) return false;
            minimum = 0x10000000ull;
            continue;
        }
        if (m_memory.map_region(base, STORAGE_SIZE,
                                MemoryProtection::ReadWrite).is_ok()) {
                    m_region_base = base;
            m_region_end = base + STORAGE_SIZE;
            // Offsets and their disjointness: kVtableLayout above.
            m_device_vtable = base + kVtableDevice;
            m_context_vtable = base + kVtableContext;
            m_dxgi_device_vtable = base + kVtableDxgiDevice;
            m_buffer_vtable = base + kVtableBuffer;
            m_texture_2d_vtable = base + kVtableTexture2d;
            m_dxgi_resource_vtable = base + kVtableDxgiResource;
            m_keyed_mutex_vtable = base + kVtableKeyedMutex;
            m_shader_resource_view_vtable = base + kVtableShaderResourceView;
            m_unordered_access_view_vtable = base + kVtableUnorderedAccessView;
            m_render_target_view_vtable = base + kVtableRenderTargetView;
            m_depth_stencil_view_vtable = base + kVtableDepthStencilView;
            m_query_vtable = base + kVtableQuery;
            m_fence_vtable = base + kVtableFence;
            m_command_list_vtable = base + kVtableCommandList;
            m_swap_chain_vtable = base + kVtableSwapChain;
            m_annotation_vtable = base + kVtableAnnotation;
            m_vertex_shader_vtable = base + kVtableVertexShader;
            m_pixel_shader_vtable = base + kVtablePixelShader;
            m_compute_shader_vtable = base + kVtableComputeShader;
            m_geometry_shader_vtable = base + kVtableGeometryShader;
            m_dxgi_surface_vtable = base + kVtableDxgiSurface;
            m_device_context_state_vtable = base + kVtableDeviceContextState;
            m_hull_shader_vtable = base + kVtableHullShader;
            m_domain_shader_vtable = base + kVtableDomainShader;
            m_input_layout_vtable = base + kVtableInputLayout;
            m_blend_state_vtable = base + kVtableBlendState;
            m_rasterizer_state_vtable = base + kVtableRasterizerState;
            m_depth_stencil_state_vtable = base + kVtableDepthStencilState;
            m_sampler_state_vtable = base + kVtableSamplerState;
            m_video_context_vtable = base + kVtableVideoContext;
            m_swap_chain_media_vtable = base + kVtableSwapChainMedia;
            m_multithread_vtable = base + kVtableMultithread;
            m_class_linkage_vtable = base + kVtableClassLinkage;
            m_class_instance_vtable = base + kVtableClassInstance;
            m_d3d10_device_vtable = base + kVtableD3D10Device;
            m_d3d10_buffer_vtable = base + kVtableD3D10Buffer;
            m_d3d10_texture_vtable = base + kVtableD3D10Texture;
            m_d3d10_shader_resource_view_vtable =
                base + kVtableD3D10ShaderResourceView;
            m_d3d10_render_target_view_vtable =
                base + kVtableD3D10RenderTargetView;
            m_d3d10_depth_stencil_view_vtable =
                base + kVtableD3D10DepthStencilView;
            m_d3d10_blend_state_vtable = base + kVtableD3D10BlendState;
            m_d3d10_depth_stencil_state_vtable =
                base + kVtableD3D10DepthStencilState;
            m_d3d10_rasterizer_state_vtable =
                base + kVtableD3D10RasterizerState;
            m_d3d10_sampler_state_vtable = base + kVtableD3D10SamplerState;
            m_d3d10_device_child_vtable = base + kVtableD3D10DeviceChild;
            m_d3d10_query_vtable = base + kVtableD3D10Query;
            m_d3d10_blob_vtable = base + kVtableD3D10Blob;
            m_d3d10_state_block_vtable = base + kVtableD3D10StateBlock;
            m_next_object = base + kVtableRegionEnd;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;

    const usize pointer_size = m_is_64bit ? sizeof(u64) : sizeof(u32);
    const auto write_vtable =
        [&](GuestAddr address, const auto& thunks) -> bool {
            for (usize index = 0; index < thunks.size(); ++index) {
                if (thunks[index] == 0 ||
                    !write_pointer(address + index * pointer_size,
                                   thunks[index])) {
                    return false;
                }
            }
            return true;
        };
    m_storage_ready =
        write_vtable(m_device_vtable, m_method_thunks.device) &&
        write_vtable(m_context_vtable, m_method_thunks.context) &&
        write_vtable(m_fence_vtable, m_method_thunks.fence) &&
        write_vtable(m_command_list_vtable, m_method_thunks.command_list) &&
        write_vtable(m_dxgi_device_vtable, m_method_thunks.dxgi_device) &&
        write_vtable(m_buffer_vtable, m_method_thunks.buffer) &&
        write_vtable(m_texture_2d_vtable, m_method_thunks.texture_2d) &&
        write_vtable(m_dxgi_resource_vtable,
                     m_method_thunks.dxgi_resource) &&
        (m_method_thunks.keyed_mutex[0] == 0 ||
         write_vtable(m_keyed_mutex_vtable, m_method_thunks.keyed_mutex)) &&
        write_vtable(m_shader_resource_view_vtable,
                     m_method_thunks.shader_resource_view) &&
        write_vtable(m_unordered_access_view_vtable,
                     m_method_thunks.unordered_access_view) &&
        write_vtable(m_render_target_view_vtable,
                     m_method_thunks.render_target_view) &&
        write_vtable(m_depth_stencil_view_vtable,
                     m_method_thunks.depth_stencil_view) &&
        write_vtable(m_query_vtable, m_method_thunks.query) &&
        write_vtable(m_swap_chain_vtable, m_method_thunks.swap_chain) &&
        write_vtable(m_annotation_vtable, m_method_thunks.annotation) &&
        write_vtable(m_vertex_shader_vtable, m_method_thunks.vertex_shader) &&
        write_vtable(m_pixel_shader_vtable, m_method_thunks.pixel_shader) &&
        write_vtable(m_compute_shader_vtable, m_method_thunks.compute_shader) &&
        write_vtable(m_geometry_shader_vtable,
                     m_method_thunks.geometry_shader) &&
        write_vtable(m_hull_shader_vtable, m_method_thunks.hull_shader) &&
        write_vtable(m_domain_shader_vtable, m_method_thunks.domain_shader) &&
        write_vtable(m_dxgi_surface_vtable, m_method_thunks.dxgi_surface) &&
        write_vtable(m_device_context_state_vtable,
                     m_method_thunks.device_context_state) &&
        write_vtable(m_input_layout_vtable, m_method_thunks.input_layout) &&
        write_vtable(m_blend_state_vtable, m_method_thunks.blend_state) &&
        write_vtable(m_rasterizer_state_vtable,
                     m_method_thunks.rasterizer_state) &&
        write_vtable(m_depth_stencil_state_vtable,
                     m_method_thunks.depth_stencil_state) &&
        write_vtable(m_sampler_state_vtable, m_method_thunks.sampler_state) &&
        write_vtable(m_video_context_vtable, m_method_thunks.video_context) &&
        write_vtable(m_swap_chain_media_vtable,
                     m_method_thunks.swap_chain_media) &&
        // Optional like the keyed mutex's, so a harness that wires only the
        // interfaces it drives still gets storage.
        (m_method_thunks.multithread[0] == 0 ||
         write_vtable(m_multithread_vtable, m_method_thunks.multithread)) &&
        (m_method_thunks.class_linkage[0] == 0 ||
         write_vtable(m_class_linkage_vtable,
                      m_method_thunks.class_linkage)) &&
        (m_method_thunks.class_instance[0] == 0 ||
         write_vtable(m_class_instance_vtable,
                      m_method_thunks.class_instance));
    // The D3D10 facets, as one optional set: without them no object
    // answers a D3D10 interface.
    m_d3d10_ready =
        m_storage_ready && m_method_thunks.d3d10_device[0] != 0 &&
        write_vtable(m_d3d10_device_vtable, m_method_thunks.d3d10_device) &&
        write_vtable(m_d3d10_buffer_vtable, m_method_thunks.d3d10_buffer) &&
        write_vtable(m_d3d10_texture_vtable, m_method_thunks.d3d10_texture) &&
        write_vtable(m_d3d10_shader_resource_view_vtable,
                     m_method_thunks.d3d10_shader_resource_view) &&
        write_vtable(m_d3d10_render_target_view_vtable,
                     m_method_thunks.d3d10_render_target_view) &&
        write_vtable(m_d3d10_depth_stencil_view_vtable,
                     m_method_thunks.d3d10_depth_stencil_view) &&
        write_vtable(m_d3d10_blend_state_vtable,
                     m_method_thunks.d3d10_blend_state) &&
        write_vtable(m_d3d10_depth_stencil_state_vtable,
                     m_method_thunks.d3d10_depth_stencil_state) &&
        write_vtable(m_d3d10_rasterizer_state_vtable,
                     m_method_thunks.d3d10_rasterizer_state) &&
        write_vtable(m_d3d10_sampler_state_vtable,
                     m_method_thunks.d3d10_sampler_state) &&
        write_vtable(m_d3d10_device_child_vtable,
                     m_method_thunks.d3d10_device_child) &&
        write_vtable(m_d3d10_query_vtable, m_method_thunks.d3d10_query) &&
        write_vtable(m_d3d10_blob_vtable, m_method_thunks.d3d10_blob) &&
        write_vtable(m_d3d10_state_block_vtable,
                     m_method_thunks.d3d10_state_block);
    return m_storage_ready;
}

bool D3D11Manager::grow_object_storage() {
    // A D3D11 interface pointer is just an address holding a vtable pointer,
    // and nothing keys off which chunk it came from, so the arena does not
    // have to be contiguous. One fixed chunk caps the process at a few
    // thousand live objects; UE5 allocates a uniform buffer per material
    // per frame-graph pass and blows through that during start-up, which
    // surfaces to the guest as E_OUTOFMEMORY from CreateBuffer.
    for (GuestAddr minimum = m_region_end; minimum != 0;) {
        const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0) {
            return false;
        }
        if (m_memory.map_region(base, STORAGE_SIZE,
                                MemoryProtection::ReadWrite).is_ok()) {
            m_next_object = base;
            m_region_end = base + STORAGE_SIZE;
            return true;
        }
        minimum = base + STORAGE_SIZE;
    }
    return false;
}

bool D3D11Manager::object_slot_live(GuestAddr address) const {
    if (address == 0) return false;
    return m_devices.contains(address) || m_interfaces.contains(address) ||
           m_buffers.contains(address) || m_textures.contains(address) ||
           m_shader_resource_views.contains(address) ||
           m_unordered_access_views.contains(address) ||
           m_render_target_views.contains(address) ||
           m_depth_stencil_views.contains(address) ||
           m_queries.contains(address) || m_fences.contains(address) ||
           m_command_lists.contains(address) ||
           m_swap_chains.contains(address) ||
           m_vertex_shaders.contains(address) ||
           m_pixel_shaders.contains(address) ||
           m_compute_shaders.contains(address) ||
           m_geometry_shaders.contains(address) ||
           m_hull_shaders.contains(address) ||
           m_domain_shaders.contains(address) ||
           m_input_layouts.contains(address) ||
           m_blend_states.contains(address) ||
           m_rasterizer_states.contains(address) ||
           m_depth_stencil_states.contains(address) ||
           m_sampler_states.contains(address) ||
           m_device_context_states.contains(address) ||
           m_class_linkages.contains(address) ||
           m_class_instances.contains(address) ||
           m_subresource_surfaces.contains(address);
}

GuestAddr D3D11Manager::allocate_object(GuestAddr vtable) {
    if (!ensure_storage()) return 0;
    GuestAddr address = 0;
    // A slot is freed by more than one path. A second free used to push the
    // same address again, so the next two creations both received it and the
    // second one's descriptor never replaced the first. Skip those duplicates,
    // and never revive an address that still names a live object.
    while (!m_free_objects.empty()) {
        address = m_free_objects.back();
        m_free_objects.pop_back();
        m_free_object_set.erase(address);
        if (address != 0 && !object_slot_live(address)) break;
        address = 0;
    }
    if (address == 0) {
        if (m_next_object + 0x40 > m_region_end && !grow_object_storage()) {
            std::fprintf(stderr,
                "[d3d11] object storage exhausted at 0x%llx (no free gap for "
                "another chunk)\n",
                static_cast<unsigned long long>(m_next_object));
            return 0;
        }
        address = m_next_object;
        m_next_object += 0x40;
    }
    const std::array<u8, 0x40> cleared{};
    if (m_memory.write_bytes(address, cleared.data(), cleared.size()).is_err() ||
        !write_pointer(address, vtable)) {
        free_object(address);
        std::fprintf(stderr, "[d3d11] object interface write failed\n");
        return 0;
    }
    return address;
}

void D3D11Manager::free_object(GuestAddr address) {
    if (address == 0) return;
    // Recycle the address first, so the guest Release below cannot observe a
    // half-freed object, then drop anything retained against it. Learn
    // (SetPrivateDataInterface): the object releases the interface when it is
    // destroyed. One slot, one free-list entry: a second release of the same
    // address must not hand that identity to two live objects.
    if (m_free_object_set.insert(address).second) {
        m_free_objects.push_back(address);
    }
    // A recycled address may name a different object next: no memoized
    // context lookup survives it (m_context_lookup_generation).
    invalidate_context_lookups();
    d3d10_forget_facet(address);
    release_private_data_interfaces(address);
}

} // namespace vitreo::winapi
