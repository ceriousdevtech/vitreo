// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_child_refcount.cpp
/// @brief Reference counts of device children: visit_child, retain and release
///        of objects and of their parents, the retain_child / release_child
///        instantiations, and the per-kind release functions. Split out of
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

// -----------------------------------------------------------------------------
// Reference counts of device children (see retain_object in d3d11.hpp)
// -----------------------------------------------------------------------------

template <typename F>
bool D3D11Manager::visit_child(GuestAddr address, F&& f) {
    if (address == 0) return false;
    const auto visit = [&](auto& objects) {
        auto it = objects.find(address);
        if (it == objects.end()) return false;
        f(it->second);
        return true;
    };
    return visit(m_buffers) || visit(m_textures) ||
           visit(m_shader_resource_views) ||
           visit(m_unordered_access_views) || visit(m_render_target_views) ||
           visit(m_depth_stencil_views) || visit(m_vertex_shaders) ||
           visit(m_pixel_shaders) || visit(m_compute_shaders) ||
           visit(m_geometry_shaders) || visit(m_hull_shaders) ||
           visit(m_domain_shaders) || visit(m_input_layouts) ||
           visit(m_blend_states) || visit(m_rasterizer_states) ||
           visit(m_depth_stencil_states) || visit(m_sampler_states) ||
           visit(m_queries) || visit(m_command_lists) || visit(m_fences) ||
           visit(m_device_context_states) || visit(m_class_linkages) ||
           visit(m_class_instances);
}

namespace {

/// A swap chain's buffer counts on its chain, every other child on the device.
template <typename T>
[[nodiscard]] GuestAddr swap_chain_parent_of(const T& object) {
    if constexpr (requires { object.swap_chain_owner; }) {
        return object.swap_chain_owner;
    } else {
        return 0;
    }
}

} // namespace

void D3D11Manager::acquire_parent(GuestAddr device_owner,
                                  GuestAddr swap_chain) {
    if (swap_chain != 0) {
        if (auto chain = m_swap_chains.find(swap_chain);
            chain != m_swap_chains.end()) {
            ++chain->second.ref_count;
        }
        return;
    }
    if (auto device = m_devices.find(device_owner);
        device != m_devices.end()) {
        (void)add_ref(device->second.device_interface);
    }
}

void D3D11Manager::release_parent(GuestAddr device_owner,
                                  GuestAddr swap_chain) {
    if (swap_chain != 0) {
        (void)release_swap_chain(swap_chain);
        return;
    }
    release_device_reference(device_owner);
}

u32 D3D11Manager::retain_object(GuestAddr address) {
    if (address == 0) return 0;
    u32 count = 0;
    bool first = false;
    GuestAddr device_owner = 0;
    GuestAddr swap_chain = 0;
    const bool child = visit_child(address, [&](auto& object) {
        ++object.ref_count;
        first = object.public_refs++ == 0;
        count = object.public_refs;
        device_owner = object.device_owner;
        swap_chain = swap_chain_parent_of(object);
    });
    if (!child) return add_ref(address);
    // The application holds the object again: it holds its parent again.
    if (first) acquire_parent(device_owner, swap_chain);
    return count;
}

u32 D3D11Manager::release_object(GuestAddr address) {
    if (address == 0) return 0;
    u32 count = 0;
    bool owned = false;
    GuestAddr device_owner = 0;
    GuestAddr swap_chain = 0;
    const bool child = visit_child(address, [&](auto& object) {
        // A Release past the application's last reference is its bug; it
        // must not take one the runtime holds (a binding, a view).
        if (object.public_refs == 0) return;
        owned = true;
        count = --object.public_refs;
        device_owner = object.device_owner;
        swap_chain = swap_chain_parent_of(object);
    });
    if (!child) return release(address);
    if (!owned) return 0;
    release_internal(address);
    // Last: this can destroy the device (or the chain).
    if (count == 0) release_parent(device_owner, swap_chain);
    return count;
}

template <typename T>
u32 D3D11Manager::retain_child(T& object) {
    // retain_object's child branch, without visit_child's walk of the maps.
    ++object.ref_count;
    const bool first = object.public_refs++ == 0;
    const u32 count = object.public_refs;
    // The application holds the object again: it holds its parent again.
    if (first) acquire_parent(object.device_owner, swap_chain_parent_of(object));
    return count;
}

template <typename T>
u32 D3D11Manager::release_child(GuestAddr address, T& object,
                                u32 (D3D11Manager::*release_kind)(GuestAddr)) {
    // release_object's child branch. A Release past the application's last
    // reference is its bug; it must not take one the runtime holds.
    if (object.public_refs == 0) return 0;
    const u32 count = --object.public_refs;
    // Copied first: the release below can destroy the object.
    const GuestAddr device_owner = object.device_owner;
    const GuestAddr swap_chain = swap_chain_parent_of(object);
    (void)(this->*release_kind)(address);
    // Last: this can destroy the device (or the chain).
    if (count == 0) release_parent(device_owner, swap_chain);
    return count;
}

template u32 D3D11Manager::retain_child(Buffer&);
template u32 D3D11Manager::retain_child(ShaderResourceView&);
template u32 D3D11Manager::release_child(
    GuestAddr, Buffer&, u32 (D3D11Manager::*)(GuestAddr));
template u32 D3D11Manager::release_child(
    GuestAddr, ShaderResourceView&, u32 (D3D11Manager::*)(GuestAddr));

void D3D11Manager::release_internal(GuestAddr address) {
    if (address == 0) return;
    if (m_buffers.contains(address)) { (void)release_buffer(address); return; }
    if (m_textures.contains(address)) { (void)release_texture(address); return; }
    if (m_shader_resource_views.contains(address)) {
        (void)release_shader_resource_view(address);
        return;
    }
    if (m_unordered_access_views.contains(address)) {
        (void)release_unordered_access_view(address);
        return;
    }
    if (m_render_target_views.contains(address)) {
        (void)release_render_target_view(address);
        return;
    }
    if (m_depth_stencil_views.contains(address)) {
        (void)release_depth_stencil_view(address);
        return;
    }
    if (m_vertex_shaders.contains(address)) {
        (void)release_vertex_shader(address);
        return;
    }
    if (m_pixel_shaders.contains(address)) {
        (void)release_pixel_shader(address);
        return;
    }
    if (m_compute_shaders.contains(address)) {
        (void)release_compute_shader(address);
        return;
    }
    if (m_geometry_shaders.contains(address)) {
        (void)release_geometry_shader(address);
        return;
    }
    if (m_hull_shaders.contains(address)) {
        (void)release_hull_shader(address);
        return;
    }
    if (m_domain_shaders.contains(address)) {
        (void)release_domain_shader(address);
        return;
    }
    if (m_input_layouts.contains(address)) {
        (void)release_input_layout(address);
        return;
    }
    if (m_blend_states.contains(address)) {
        (void)release_blend_state(address);
        return;
    }
    if (m_rasterizer_states.contains(address)) {
        (void)release_rasterizer_state(address);
        return;
    }
    if (m_depth_stencil_states.contains(address)) {
        (void)release_depth_stencil_state(address);
        return;
    }
    if (m_sampler_states.contains(address)) {
        (void)release_sampler_state(address);
        return;
    }
    if (m_queries.contains(address)) { (void)release_query(address); return; }
    if (m_command_lists.contains(address)) {
        (void)release_command_list(address);
        return;
    }
    if (m_fences.contains(address)) { (void)release_fence(address); return; }
    if (m_device_context_states.contains(address)) {
        (void)release_device_context_state(address);
        return;
    }
    if (m_class_linkages.contains(address)) {
        (void)release_class_linkage(address);
        return;
    }
    if (m_class_instances.contains(address)) {
        (void)release_class_instance(address);
    }
}

GuestAddr D3D11Manager::device_owner_of(GuestAddr address) {
    GuestAddr owner = 0;
    (void)visit_child(address, [&owner](const auto& object) {
        owner = object.device_owner;
    });
    return owner;
}

bool D3D11Manager::hold_object(GuestAddr address) {
    return visit_child(address, [](auto& object) { ++object.ref_count; });
}

void D3D11Manager::unhold_object(GuestAddr address) {
    release_internal(address);
}

void D3D11Manager::release_device_reference(GuestAddr owner) {
    auto device = m_devices.find(owner);
    if (device != m_devices.end()) {
        (void)release(device->second.device_interface);
    }
}

u32 D3D11Manager::release_buffer(GuestAddr address) {
    auto buffer = m_buffers.find(address);
    if (buffer == m_buffers.end() || buffer->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --buffer->second.ref_count;
    if (count != 0) return count;
    if (buffer->second.mapped_address != 0) {
        (void)m_memory.unmap_region(
            buffer->second.mapped_address, buffer->second.mapped_size);
    }
    // An opening destroyed while it owns the keyed mutex abandons it.
    if (buffer->second.keyed_mutex) {
        buffer->second.keyed_mutex->opening_destroyed(
            buffer->second.keyed_mutex_opening);
    }
    BufferTombstone& tomb = m_buffer_tombstones[address];
    tomb.desc = buffer->second.desc;
    tomb.final_release_caller = m_releasing_caller;
    auto created = m_buffer_create_callers.find(address);
    tomb.create_caller =
        created != m_buffer_create_callers.end() ? created->second : 0;
    m_buffers.erase(buffer);
    free_object(address);
    return 0;
}

u32 D3D11Manager::release_texture(GuestAddr address) {
    auto texture = m_textures.find(address);
    if (texture == m_textures.end() || texture->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --texture->second.ref_count;
    const GuestAddr swap_chain = texture->second.swap_chain_owner;
    if (count != 0) {
        if (count == 1 && swap_chain != 0) {
            destroy_swap_chain_if_unreferenced(swap_chain);
        }
        return count;
    }
    for (const auto& storage : texture->second.subresources) {
        if (storage.mapped_address != 0) {
            (void)m_memory.unmap_region(
                storage.mapped_address, storage.mapped_size);
        }
    }
    // An opening destroyed while it owns the keyed mutex abandons it
    // (measured: the next AcquireSync answers WAIT_ABANDONED).
    if (texture->second.keyed_mutex) {
        texture->second.keyed_mutex->opening_destroyed(
            texture->second.keyed_mutex_opening);
    }
    m_textures.erase(texture);
    free_object(address);
    return 0;
}

u32 D3D11Manager::release_shader_resource_view(GuestAddr address) {
    auto view = m_shader_resource_views.find(address);
    if (view == m_shader_resource_views.end() ||
        view->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --view->second.ref_count;
    if (count != 0) return count;
    const GuestAddr texture = view->second.texture;
    // A buffer view holds a reference on the buffer instead of a texture.
    const GuestAddr buffer = view->second.buffer;
    m_shader_resource_views.erase(view);
    ++m_srv_set_generation;
    forget_srv_memo(address);
    free_object(address);
    if (buffer != 0) {
        (void)release_buffer(buffer);
        return 0;
    }
    (void)release_texture(texture);
    return 0;
}

u32 D3D11Manager::release_unordered_access_view(GuestAddr address) {
    auto view = m_unordered_access_views.find(address);
    if (view == m_unordered_access_views.end() || view->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --view->second.ref_count;
    if (count != 0) return count;
    const GuestAddr texture = view->second.texture;
    // A buffer view holds a reference on the buffer instead of a texture.
    const GuestAddr buffer = view->second.buffer;
    m_unordered_access_views.erase(view);
    free_object(address);
    if (buffer != 0) {
        (void)release_buffer(buffer);
        return 0;
    }
    (void)release_texture(texture);
    return 0;
}

u32 D3D11Manager::release_render_target_view(GuestAddr address) {
    auto view = m_render_target_views.find(address);
    if (view == m_render_target_views.end() || view->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --view->second.ref_count;
    if (count != 0) return count;
    const GuestAddr texture = view->second.texture;
    // A buffer view holds its reference on the buffer.
    const GuestAddr buffer = view->second.buffer;
    m_render_target_views.erase(view);
    // A bound view retains a reference; only the final Release permits reuse.
    free_object(address);
    if (buffer != 0) {
        (void)release_buffer(buffer);
        return 0;
    }
    (void)release_texture(texture);
    return 0;
}

u32 D3D11Manager::release_depth_stencil_view(GuestAddr address) {
    auto view = m_depth_stencil_views.find(address);
    if (view == m_depth_stencil_views.end() || view->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --view->second.ref_count;
    if (count != 0) return count;
    const GuestAddr texture = view->second.texture;
    m_depth_stencil_views.erase(view);
    free_object(address);
    (void)release_texture(texture);
    return 0;
}

u32 D3D11Manager::release_query(GuestAddr address) {
    auto query = m_queries.find(address);
    if (query == m_queries.end() || query->second.ref_count == 0) {
        return 0;
    }
    const u32 count = --query->second.ref_count;
    if (count != 0) return count;
    // A query released while building leaves the counting list of its
    // context; its occlusion counters return their slots once the GPU has
    // retired the draws that counted into them.
    stop_counting(query->second);
    m_queries.erase(query);
    free_object(address);
    return 0;
}

} // namespace vitreo::winapi
