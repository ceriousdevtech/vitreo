// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_view_create.cpp
/// @brief Creation of shader-resource, unordered-access, render-target and
///        depth-stencil views, and of queries. Split out of d3d11.cpp as
///        verbatim code motion.
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

GuestAddr D3D11Manager::create_shader_resource_view(
    GuestAddr device_owner, GuestAddr texture_address, u32 format,
    u32 view_dimension, u32 most_detailed_mip, u32 mip_levels,
    u32 first_array_slice, u32 array_size) {
    auto texture = m_textures.find(texture_address);
    if (texture == m_textures.end() ||
        texture->second.device_owner != device_owner ||
        (texture->second.desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
        mip_levels == 0 ||
        most_detailed_mip >= texture->second.desc.mip_levels ||
        mip_levels > texture->second.desc.mip_levels - most_detailed_mip ||
        array_size == 0 ||
        first_array_slice >= texture->second.desc.array_size ||
        array_size > texture->second.desc.array_size - first_array_slice ||
        !ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_shader_resource_view_vtable);
    if (address == 0) return 0;
    // The IDXGISurface facet an SRV answers (srv_query_interface).
    if (!write_pointer(address + 0x20, m_dxgi_surface_vtable)) {
        free_object(address);
        return 0;
    }
    ShaderResourceView view{};
    view.interface_address = address;
    view.dxgi_surface_interface = address + 0x20;
    view.device_owner = device_owner;
    view.texture = texture_address;
    view.format = format;
    view.view_dimension = view_dimension;
    view.most_detailed_mip = most_detailed_mip;
    view.mip_levels = mip_levels;
    view.first_array_slice = first_array_slice;
    view.array_size = array_size;
    m_shader_resource_views.emplace(address, std::move(view));
    ++m_srv_set_generation;
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++texture->second.ref_count;
    return address;
}

GuestAddr D3D11Manager::create_buffer_shader_resource_view(
    GuestAddr device_owner, GuestAddr buffer_address, u32 format,
    u32 view_dimension, u32 first_element, u32 num_elements,
    u32 element_bytes,
    std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer) {
    auto buffer = m_buffers.find(buffer_address);
    if (buffer == m_buffers.end() ||
        buffer->second.device_owner != device_owner ||
        (buffer->second.desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
        num_elements == 0 || element_bytes == 0 ||
        !ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_shader_resource_view_vtable);
    if (address == 0) return 0;
    // The IDXGISurface facet an SRV answers (srv_query_interface).
    if (!write_pointer(address + 0x20, m_dxgi_surface_vtable)) {
        free_object(address);
        return 0;
    }
    ShaderResourceView view{};
    view.interface_address = address;
    view.dxgi_surface_interface = address + 0x20;
    view.device_owner = device_owner;
    view.buffer = buffer_address;
    view.format = format;
    view.view_dimension = view_dimension;
    view.first_element = first_element;
    view.num_elements = num_elements;
    view.element_bytes = element_bytes;
    view.texture_buffer = std::move(texture_buffer);
    m_shader_resource_views.emplace(address, std::move(view));
    ++m_srv_set_generation;
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++buffer->second.ref_count;
    return address;
}

GuestAddr D3D11Manager::create_buffer_unordered_access_view(
    GuestAddr device_owner, GuestAddr buffer_address, u32 format,
    u32 view_dimension, u32 first_element, u32 num_elements,
    u32 element_bytes, u32 buffer_flags,
    std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer) {
    auto buffer = m_buffers.find(buffer_address);
    if (buffer == m_buffers.end() ||
        buffer->second.device_owner != device_owner ||
        (buffer->second.desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) == 0 ||
        num_elements == 0 || element_bytes == 0 ||
        !ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_unordered_access_view_vtable);
    if (address == 0) return 0;
    UnorderedAccessView view{};
    view.interface_address = address;
    view.device_owner = device_owner;
    view.buffer = buffer_address;
    view.format = format;
    view.view_dimension = view_dimension;
    view.first_element = first_element;
    view.num_elements = num_elements;
    view.element_bytes = element_bytes;
    view.buffer_flags = buffer_flags;
    view.texture_buffer = std::move(texture_buffer);
    if ((buffer_flags & (d3d11::BUFFER_UAV_FLAG_APPEND |
                         d3d11::BUFFER_UAV_FLAG_COUNTER)) != 0) {
        // Four bytes of GPU storage for the hidden counter. It has to be a real
        // buffer, not a number in this record: the shader increments it and
        // CopyStructureCount reads it, and both happen on the GPU timeline.
        graphics::BackendBufferDesc counter_desc{};
        counter_desc.byte_width = sizeof(u32);
        counter_desc.unordered_access = true;
        counter_desc.shader_resource = true;
        counter_desc.cpu_writable = true;
        const u32 zero = 0;
        view.counter = m_backend.create_buffer(counter_desc, &zero);
        if (!view.counter || !view.counter->valid()) {
            // A view whose counter does not exist would report a count that
            // nothing maintains, so creation fails instead.
            free_object(address);
            return 0;
        }
    }
    m_unordered_access_views.emplace(address, std::move(view));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++buffer->second.ref_count;
    return address;
}

bool D3D11Manager::apply_uav_initial_count(GuestAddr view_address,
                                           u32 initial_count) {
    // Learn: -1 keeps the current offset, and the value is only relevant for an
    // APPEND or COUNTER view. Anything else is silently ignored by the runtime,
    // so a guest passing counts for plain UAVs must not fail here.
    if (initial_count == 0xFFFFFFFFu) return true;
    auto view = m_unordered_access_views.find(view_address);
    if (view == m_unordered_access_views.end() || !view->second.counter) {
        return true;
    }
    // On the command timeline, like every other write to the counter: a
    // CopyStructureCount recorded before this rebind must still read the
    // count the earlier dispatches left (a09b probe [append/consume]: 9, not
    // the new 7). Writing the Shared counter from the CPU here overtook it.
    graphics::BackendBufferDesc value_desc{};
    value_desc.byte_width = sizeof(u32);
    auto value = m_backend.create_buffer(value_desc, &initial_count);
    if (!value || !value->valid()) return false;
    return m_backend.copy_buffer_region(view->second.counter.get(), 0,
                                        value.get(), 0, sizeof(u32));
}

GuestAddr D3D11Manager::create_unordered_access_view(
    GuestAddr device_owner, GuestAddr texture_address, u32 format,
    u32 view_dimension, u32 mip_slice, u32 first_w_slice, u32 w_size) {
    auto texture = m_textures.find(texture_address);
    // Learn D3D11_TEX3D_UAV: a volume view selects a mip and a W-slice
    // window of it, bounded by that mip's depth (WSize -1 = every remaining
    // slice). The dimension is a resource property: TEXTURE3D only over a
    // Texture3D and TEXTURE2D only over a Texture2D.
    const bool volume_view = view_dimension == D3D11_UAV_DIMENSION_TEXTURE3D;
    const bool linear_view =
        view_dimension == D3D11_UAV_DIMENSION_TEXTURE1D ||
        view_dimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
    const u32 mip_depth = texture == m_textures.end() || !volume_view ? 1u
        : std::max(1u, texture->second.volume_desc.depth >> mip_slice);
    if (volume_view && w_size == 0xFFFFFFFFu) {
        w_size = first_w_slice < mip_depth ? mip_depth - first_w_slice : 0u;
    }
    // D3D11_TEX1D_ARRAY_UAV and D3D11_TEX2D_ARRAY_UAV arrive in the same
    // two union slots.
    const bool array_view =
        view_dimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY ||
        view_dimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    if (texture == m_textures.end() ||
        texture->second.device_owner != device_owner ||
        (texture->second.desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) == 0 ||
        volume_view != texture->second.volume ||
        (volume_view && !texture->second.backend_3d) ||
        (volume_view && (w_size == 0 || first_w_slice >= mip_depth ||
                         w_size > mip_depth - first_w_slice)) ||
        texture->second.desc.sample_count != 1 ||
        // A 1D view names a 1D resource and a 2D view a 2D one, in both
        // directions. The backing texture for a Texture1D really is a height-1
        // 2D one, so accepting a 2D view of it would work here and then bind to
        // a shader register that cannot address what the guest asked for.
        (!linear_view && !volume_view &&
         view_dimension != D3D11_UAV_DIMENSION_TEXTURE2D &&
         view_dimension != D3D11_UAV_DIMENSION_TEXTURE2DARRAY) ||
        (linear_view != texture->second.linear) ||
        (array_view && !uav_array_window_valid(
                           texture->second.desc.array_size, first_w_slice,
                           w_size)) ||
        mip_slice >= texture->second.desc.mip_levels ||
        // Learn D3D11_UNORDERED_ACCESS_VIEW_DESC: a view of a typeless
        // resource supplies the interpretation, so the view format only has
        // to be a typed member of the resource's DXGI family — requiring
        // equality refused every UAV over a typeless render target, which is
        // how an engine declares one resource it both renders and computes
        // into. A 32-bit typeless texture also takes an R32 cast (spec
        // §5.3.9.5), which the backend binds as a pixel-format view.
        (dxgi_format_has_video_views(texture->second.desc.format)
             ? dxgi_video_view_plane(texture->second.desc.format, format,
                                     true) < 0
             : !dxgi_uav_view_compatible(texture->second.desc.format,
                                         format)) ||
        !ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_unordered_access_view_vtable);
    if (address == 0) return 0;
    UnorderedAccessView view{};
    view.interface_address = address;
    view.device_owner = device_owner;
    view.texture = texture_address;
    view.format = format;
    view.view_dimension = view_dimension;
    view.mip_slice = mip_slice;
    view.first_w_slice = volume_view ? first_w_slice : 0u;
    view.w_size = volume_view ? w_size : 0u;
    if (array_view) {
        view.first_array_slice = first_w_slice;
        view.array_size = w_size == 0xFFFFFFFFu
            ? texture->second.desc.array_size - first_w_slice
            : w_size;
    }
    m_unordered_access_views.emplace(address, std::move(view));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++texture->second.ref_count;
    return address;
}

GuestAddr D3D11Manager::create_buffer_render_target_view(
    GuestAddr device_owner, GuestAddr buffer_address, u32 format,
    u32 first_element, u32 num_elements, u32 element_bytes) {
    auto buffer = m_buffers.find(buffer_address);
    if (buffer == m_buffers.end() ||
        buffer->second.device_owner != device_owner ||
        (buffer->second.desc.bind_flags & D3D11_BIND_RENDER_TARGET) == 0 ||
        !buffer->second.backend || num_elements == 0 || element_bytes == 0 ||
        !ensure_storage()) {
        return 0;
    }
    graphics::BackendTexture2DDesc proxy{};
    proxy.width = num_elements;
    proxy.height = 1;
    proxy.mip_levels = proxy.array_size = 1;
    proxy.pixel_format = backend_format(format);
    proxy.render_target = true;
    auto target = m_backend.create_texture_2d(proxy, nullptr, 0);
    if (!target || !target->valid()) return 0;
    const GuestAddr address = allocate_object(m_render_target_view_vtable);
    if (address == 0) return 0;
    RenderTargetView view{};
    view.interface_address = address;
    view.device_owner = device_owner;
    view.buffer = buffer_address;
    view.format = format;
    view.view_dimension = D3D11_RTV_DIMENSION_BUFFER;
    view.first_element = first_element;
    view.num_elements = num_elements;
    view.buffer_proxy = std::move(target);
    m_render_target_views.emplace(address, std::move(view));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++buffer->second.ref_count;
    return address;
}

bool D3D11Manager::sync_buffer_render_target(const RenderTargetView& view,
                                             bool into_proxy) {
    auto buffer = m_buffers.find(view.buffer);
    if (buffer == m_buffers.end() || !buffer->second.backend ||
        !view.buffer_proxy) {
        return false;
    }
    const u64 texel =
        graphics::format_table(view.buffer_proxy->desc().pixel_format)
            .bytes_per_pixel;
    if (!m_backend.copy_buffer_texture_row(
            buffer->second.backend.get(), texel * view.first_element,
            view.buffer_proxy.get(), view.num_elements, into_proxy)) {
        return false;
    }
    if (!into_proxy) buffer->second.gpu_newer = true;
    note_gpu_use(buffer->second);
    return true;
}

GuestAddr D3D11Manager::create_render_target_view(
    GuestAddr device_owner, GuestAddr texture_address, u32 format,
    u32 view_dimension, u32 mip_slice,
    u32 first_array_slice, u32 array_size) {
    auto texture = m_textures.find(texture_address);
    // A Texture3D view's window addresses W slices of the selected mip, so it
    // is bounded by the volume's depth rather than by the array size — the one
    // structural difference from every 2D view shape.
    const bool volume_view = view_dimension == D3D11_RTV_DIMENSION_TEXTURE3D;
    const u32 slice_limit =
        texture == m_textures.end() ? 0u
        : volume_view
            ? std::max(1u, texture->second.volume_desc.depth >> mip_slice)
            : texture->second.desc.array_size;
    if (texture == m_textures.end() ||
        texture->second.device_owner != device_owner ||
        (texture->second.desc.bind_flags & D3D11_BIND_RENDER_TARGET) == 0 ||
        volume_view != texture->second.volume ||
        (volume_view && !texture->second.backend_3d) ||
        (view_dimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
         view_dimension != D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
         view_dimension != D3D11_RTV_DIMENSION_TEXTURE2DMS &&
         view_dimension != D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY &&
         view_dimension != D3D11_RTV_DIMENSION_TEXTURE1D &&
         view_dimension != D3D11_RTV_DIMENSION_TEXTURE1DARRAY &&
         !volume_view) ||
        // A 1D view names a 1D resource, both ways round. The backing texture is
        // a height-1 2D one, so a 2D view of it would work here and then bind to
        // a register that cannot address what the guest asked for.
        ((view_dimension == D3D11_RTV_DIMENSION_TEXTURE1D ||
          view_dimension == D3D11_RTV_DIMENSION_TEXTURE1DARRAY) !=
         texture->second.linear) ||
        mip_slice >= texture->second.desc.mip_levels ||
        array_size == 0 ||
        first_array_slice >= slice_limit ||
        array_size > slice_limit - first_array_slice ||
        ((view_dimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
          view_dimension == D3D11_RTV_DIMENSION_TEXTURE2DMS) &&
         (first_array_slice != 0 || array_size != 1)) ||
        !ensure_storage()) {
        std::fprintf(stderr,
            "[d3d11] CreateRenderTargetView failed: resource=0x%llx "
            "found=%d dimension=%u mip=%u first=%u count=%u "
            "next_object=0x%llx chunk_end=0x%llx live_views=%zu\n",
            static_cast<unsigned long long>(texture_address),
            texture != m_textures.end(), view_dimension, mip_slice,
            first_array_slice, array_size,
            static_cast<unsigned long long>(m_next_object),
            static_cast<unsigned long long>(m_region_end),
            m_render_target_views.size());
        return 0;
    }
    const GuestAddr address = allocate_object(m_render_target_view_vtable);
    if (address == 0) return 0;
    RenderTargetView view{};
    view.interface_address = address;
    view.device_owner = device_owner;
    view.texture = texture_address;
    view.format = format == 0 ? texture->second.desc.format : format;
    view.view_dimension = view_dimension;
    view.mip_slice = mip_slice;
    view.first_array_slice = first_array_slice;
    view.array_size = array_size;
    m_render_target_views.emplace(address, std::move(view));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++texture->second.ref_count;
    return address;
}

GuestAddr D3D11Manager::create_depth_stencil_view(
    GuestAddr device_owner, GuestAddr texture_address, u32 format,
    u32 view_dimension, u32 flags, u32 mip_slice,
    u32 first_array_slice, u32 array_size) {
    auto texture = m_textures.find(texture_address);
    if (texture == m_textures.end() ||
        texture->second.device_owner != device_owner ||
        (texture->second.desc.bind_flags & D3D11_BIND_DEPTH_STENCIL) == 0 ||
        !ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_depth_stencil_view_vtable);
    if (address == 0) return 0;
    DepthStencilView view{};
    view.interface_address = address;
    view.device_owner = device_owner;
    view.texture = texture_address;
    view.format = format == 0 ? texture->second.desc.format : format;
    view.view_dimension = view_dimension;
    view.flags = flags;
    view.mip_slice = mip_slice;
    view.first_array_slice = first_array_slice;
    view.array_size = array_size;
    m_depth_stencil_views.emplace(address, std::move(view));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    ++texture->second.ref_count;
    return address;
}

GuestAddr D3D11Manager::create_query(
    GuestAddr device_owner, u32 query_type, u32 misc_flags) {
    if (!query_type_valid(query_type) || !ensure_storage()) {
        return 0;
    }
    const GuestAddr address = allocate_object(m_query_vtable);
    if (address == 0) return 0;
    Query query{};
    query.interface_address = address;
    query.device_owner = device_owner;
    query.query_type = query_type;
    query.misc_flags = misc_flags;
    query.predicate = query_type_is_predicate(query_type);
    m_queries.emplace(address, std::move(query));
    // The application's creation reference holds the device.
    acquire_parent(device_owner);
    return address;
}

} // namespace vitreo::winapi
