// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_buffers.cpp
/// @brief The DXGI swap-chain buffer ring: creation, flip-model back
///        buffer rotation, per-index buffer interfaces and
///        IDXGISwapChain::ResizeBuffers.
///        Split out of d3d11_swapchain.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_swapchain_internal.hpp"
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

bool D3D11Manager::create_swap_chain_ring(SwapChain& swap_chain,
                                          Texture2D& back_buffer) {
    swap_chain.flip_back_buffers.clear();
    swap_chain.msaa_resolve.reset();
    swap_chain.current_back_buffer_index = 0;
    if (!back_buffer.backend) return false;
    const auto physical_desc = back_buffer.backend->desc();
    // Microsoft Learn, DXGI flip model: BufferCount is the physical buffer
    // ring and Present rotates which allocation backs buffer zero. Keep the
    // guest Texture2D identity stable, but allocate the remaining physical
    // buffers so a presented IOSurface is never modified by the next frame.
    // SEQUENTIAL keeps the same ring so GetBuffer(n) has a buffer n to name.
    if (dxgi_ring_effect(swap_chain.desc.swap_effect) &&
        swap_chain.desc.buffer_count > 1) {
        swap_chain.flip_back_buffers.reserve(swap_chain.desc.buffer_count - 1);
        for (u32 index = 1; index < swap_chain.desc.buffer_count; ++index) {
            auto physical =
                m_backend.create_texture_2d(physical_desc, nullptr, 0);
            if (!physical || !physical->valid()) {
                d3d11_diag("swap chain ring buffer %u/%u failed", index + 1,
                           swap_chain.desc.buffer_count);
                swap_chain.flip_back_buffers.clear();
                return false;
            }
            swap_chain.flip_back_buffers.push_back(std::move(physical));
        }
    }
    // A multisampled bitblt back buffer is resolved into a single-sample
    // texture that Present shows.
    if (swap_chain.desc.sample_count > 1) {
        auto resolve_desc = physical_desc;
        resolve_desc.sample_count = 1;
        resolve_desc.render_target = true;
        swap_chain.msaa_resolve =
            m_backend.create_texture_2d(resolve_desc, nullptr, 0);
        if (!swap_chain.msaa_resolve || !swap_chain.msaa_resolve->valid()) {
            swap_chain.msaa_resolve.reset();
            return false;
        }
    }
    return true;
}

bool D3D11Manager::advance_flip_back_buffer(
    SwapChain& swap_chain, Texture2D& texture) {
    const u32 effect = swap_chain.desc.swap_effect;
    if (!dxgi_ring_effect(effect) || swap_chain.desc.buffer_count < 2 ||
        !texture.backend) {
        return true;
    }
    // The ring in buffer order: buffer 0 is the guest back-buffer Texture2D,
    // buffer n the GetBuffer(n) texture once one exists, else the chain's own
    // storage for it. Present moves every allocation one place towards
    // buffer 0 and the presented frame to the end, which is the D3D11
    // runtime's rename: GetBuffer(0) always names the buffer to draw next.
    std::vector<std::unique_ptr<graphics::BackendTexture2D>*> ring;
    ring.reserve(swap_chain.desc.buffer_count);
    ring.push_back(&texture.backend);
    for (u32 index = 1; index < swap_chain.desc.buffer_count; ++index) {
        std::unique_ptr<graphics::BackendTexture2D>* slot = nullptr;
        if (index - 1 < swap_chain.index_buffers.size() &&
            swap_chain.index_buffers[index - 1] != 0) {
            auto owned = m_textures.find(swap_chain.index_buffers[index - 1]);
            if (owned != m_textures.end()) slot = &owned->second.backend;
        }
        if (slot == nullptr && index - 1 < swap_chain.flip_back_buffers.size()) {
            slot = &swap_chain.flip_back_buffers[index - 1];
        }
        if (slot == nullptr || !*slot || !(*slot)->valid()) return false;
        ring.push_back(slot);
    }
    auto& next = *ring[1];
    const u64 next_handle = next->shared_handle();
    if (next_handle != 0) {
        // The allocation about to become buffer zero may still be consumed
        // by a foreign DWM/owner process from an earlier Present. Back-pressure
        // belongs here, before the guest can draw into it, not at its next
        // Present after those writes have already raced the consumer.
        wait_for_foreign_surface_reuse(next_handle);
    }
    if (dxgi_sequential_effect(effect) &&
        !m_backend.copy_texture_2d(next.get(), texture.backend.get())) {
        // FLIP_SEQUENTIAL / SEQUENTIAL guarantee preservation across
        // Present. Do not rotate to stale storage if the backend cannot carry
        // the coherent frame forward. FLIP_DISCARD intentionally skips this.
        return false;
    }
    auto presented = std::move(*ring[0]);
    for (usize i = 0; i + 1 < ring.size(); ++i) {
        *ring[i] = std::move(*ring[i + 1]);
    }
    *ring.back() = std::move(presented);
    if (dxgi_flip_effect(effect)) {
        swap_chain.current_back_buffer_index =
            (swap_chain.current_back_buffer_index + 1) %
            swap_chain.desc.buffer_count;
    }
    return true;
}

GuestAddr D3D11Manager::swap_chain_index_buffer(GuestAddr self, u32 index) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end() || index == 0) return 0;
    SwapChain& chain = found->second;
    if (index >= chain.desc.buffer_count ||
        index - 1 >= chain.flip_back_buffers.size()) {
        return 0;
    }
    if (chain.index_buffers.size() < chain.desc.buffer_count - 1) {
        chain.index_buffers.resize(chain.desc.buffer_count - 1, 0);
    }
    if (chain.index_buffers[index - 1] != 0) {
        return chain.index_buffers[index - 1];
    }
    auto back = m_textures.find(chain.back_buffer);
    if (back == m_textures.end()) return 0;
    // Buffer n is its own texture (Learn GetBuffer: read-only for n > 0),
    // backed by the ring allocation for n. The chain keeps one reference.
    Texture2DDesc desc = back->second.desc;
    const GuestAddr texture = create_texture_2d(chain.device_owner, desc,
                                                nullptr, 0, self);
    auto created = m_textures.find(texture);
    if (texture == 0 || created == m_textures.end() ||
        !chain.flip_back_buffers[index - 1]) {
        if (texture != 0) (void)release_texture(texture);
        return 0;
    }
    created->second.backend = std::move(chain.flip_back_buffers[index - 1]);
    ++m_texture_backend_gen;  // R4 srv-memo: a texture's backend replaced
    created->second.read_only_buffer = true;
    chain.index_buffers[index - 1] = texture;
    return texture;
}

void D3D11Manager::release_swap_chain_index_buffers(SwapChain& chain) {
    for (usize i = 0; i < chain.index_buffers.size(); ++i) {
        const GuestAddr texture = chain.index_buffers[i];
        if (texture == 0) continue;
        auto owned = m_textures.find(texture);
        if (owned != m_textures.end() && owned->second.backend &&
            i < chain.flip_back_buffers.size() && !chain.flip_back_buffers[i]) {
            // Hand the allocation back to the ring before the texture goes.
            chain.flip_back_buffers[i] = std::move(owned->second.backend);
        }
        chain.index_buffers[i] = 0;
        (void)release_texture(texture);
    }
    chain.index_buffers.clear();
}

void D3D11Manager::unbind_texture_views(GuestAddr texture, GuestAddr context,
                                        bool render_targets_only) {
    if (texture == 0) return;
    const auto rtv_of = [&](GuestAddr view) {
        auto found = m_render_target_views.find(view);
        return found != m_render_target_views.end() &&
               found->second.texture == texture;
    };
    const auto srv_of = [&](GuestAddr view) {
        auto found = m_shader_resource_views.find(view);
        return found != m_shader_resource_views.end() &&
               found->second.texture == texture;
    };
    const auto uav_of = [&](GuestAddr view) {
        auto found = m_unordered_access_views.find(view);
        return found != m_unordered_access_views.end() &&
               found->second.texture == texture;
    };
    for (auto& [address, state] : m_context_states) {
        if (context != 0 && address != context) continue;
        for (usize slot = 0; slot < state.render_targets.size(); ++slot) {
            const GuestAddr view = state.render_targets[slot];
            if (view != 0 && rtv_of(view)) {
                state.render_targets.set(slot, 0);
                (void)release_render_target_view(view);
            }
        }
        if (render_targets_only) continue;
        for (auto* views :
             {&state.vertex_shader_resources, &state.pixel_shader_resources,
              &state.compute_shader_resources,
              &state.geometry_shader_resources, &state.hull_shader_resources,
              &state.domain_shader_resources}) {
            for (usize slot = 0; slot < views->size(); ++slot) {
                const GuestAddr view = (*views)[slot];
                if (view != 0 && srv_of(view)) {
                    views->set(slot, 0);
                    (void)release_shader_resource_view(view);
                }
            }
        }
        for (auto* views : {&state.compute_unordered_access_views,
                            &state.output_merger_unordered_access_views}) {
            for (GuestAddr& view : *views) {
                if (view != 0 && uav_of(view)) {
                    const GuestAddr dead = view;
                    view = 0;
                    (void)release_unordered_access_view(dead);
                }
            }
        }
    }
}

bool D3D11Manager::swap_chain_buffers_referenced(const SwapChain& chain) {
    // Learn ResizeBuffers: every reference to the buffers must be released
    // first. Measured: a GetBuffer reference or a live RTV blocks it (the
    // view holds the texture); an RTV the app released but left bound to
    // the immediate context does NOT -- binding references are the
    // runtime's own. So count, per buffer, the references that are neither
    // the chain's nor a view that exists only through bindings.
    std::unordered_map<GuestAddr, u32> binding_refs;
    for (const auto& [address, state] : m_context_states) {
        (void)address;
        for (const GuestAddr view : state.render_targets) {
            if (view != 0) ++binding_refs[view];
        }
        for (const auto* views :
             {&state.vertex_shader_resources, &state.pixel_shader_resources,
              &state.compute_shader_resources,
              &state.geometry_shader_resources, &state.hull_shader_resources,
              &state.domain_shader_resources}) {
            for (const GuestAddr view : *views) {
                if (view != 0) ++binding_refs[view];
            }
        }
        for (const auto* views : {&state.compute_unordered_access_views,
                                  &state.output_merger_unordered_access_views}) {
            for (const GuestAddr view : *views) {
                if (view != 0) ++binding_refs[view];
            }
        }
    }
    const auto public_refs = [&](GuestAddr view, u32 ref_count) {
        const auto bound = binding_refs.find(view);
        const u32 held = bound == binding_refs.end() ? 0u : bound->second;
        return ref_count > held;
    };
    std::vector<GuestAddr> buffers{chain.back_buffer};
    for (const GuestAddr index : chain.index_buffers) {
        if (index != 0) buffers.push_back(index);
    }
    for (const GuestAddr buffer : buffers) {
        auto texture = m_textures.find(buffer);
        if (texture == m_textures.end()) continue;
        u32 view_refs = 0;
        for (const auto& [view, rtv] : m_render_target_views) {
            if (rtv.texture != buffer) continue;
            ++view_refs;
            if (public_refs(view, rtv.ref_count)) return true;
        }
        for (const auto& [view, srv] : m_shader_resource_views) {
            if (srv.texture != buffer) continue;
            ++view_refs;
            if (public_refs(view, srv.ref_count)) return true;
        }
        for (const auto& [view, uav] : m_unordered_access_views) {
            if (uav.texture != buffer) continue;
            ++view_refs;
            if (public_refs(view, uav.ref_count)) return true;
        }
        // One reference is the chain's own; the rest are views' or public.
        if (texture->second.ref_count > 1 + view_refs) return true;
    }
    return false;
}

u32 D3D11Manager::resize_swap_chain_buffers(GuestAddr self, u32 count,
                                            u32 width, u32 height, u32 format,
                                            u32 flags) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    SwapChain& chain = found->second;
    const bool flip = dxgi_flip_effect(chain.desc.swap_effect);
    // Rules checked before the chain changes (measured; the description is
    // left as it was):
    //  - BufferCount past DXGI_MAX_SWAP_CHAIN_BUFFERS, or 1 on a flip chain,
    //    is E_INVALIDARG (0 keeps the count);
    //  - flags creation would refuse are INVALID_CALL, except
    //    FOREGROUND_LAYER, which is E_INVALIDARG, as is adding or removing
    //    ALLOW_TEARING or FRAME_LATENCY_WAITABLE_OBJECT;
    //  - a flip chain's format outside its set is E_INVALIDARG.
    if (count > kDxgiMaxSwapChainBuffers || (flip && count == 1)) {
        return d3d11::E_INVALIDARG;
    }
    const u32 fixed_flags =
        kSwapChainFlagAllowTearing | kSwapChainFlagFrameLatencyWaitableObject;
    if (((flags ^ chain.desc.flags) & fixed_flags) != 0) {
        return d3d11::E_INVALIDARG;
    }
    const u32 allowed = (flip ? kSwapChainFlagsFlip : kSwapChainFlagsBitblt) |
                        kSwapChainFlagRestrictSharedResourceDriver |
                        kSwapChainFlagForegroundLayer;
    if ((flags & ~allowed) != 0) return DXGI_ERROR_INVALID_CALL;
    if ((flags & kSwapChainFlagForegroundLayer) != 0) {
        return d3d11::E_INVALIDARG;
    }
    if (format != 0 && flip && !dxgi_flip_swap_chain_format(format)) {
        return d3d11::E_INVALIDARG;
    }
    if (swap_chain_buffers_referenced(chain)) {
        d3d11_diag("IDXGISwapChain::ResizeBuffers chain=0x%llx -> "
                   "DXGI_ERROR_INVALID_CALL (buffer references outstanding)",
                   static_cast<unsigned long long>(self));
        return DXGI_ERROR_INVALID_CALL;
    }
    // Learn: a zero Width or Height takes the window's client area -- both
    // dimensions when either is zero (measured: 0x50 gave 176x132).
    if (width == 0 || height == 0) {
        const auto client = chain.composition
            ? std::nullopt
            : window_client_size(chain.desc.output_window);
        if (client) {
            width = client->first;
            height = client->second;
        } else {
            width = chain.desc.width;
            height = chain.desc.height;
        }
    }
    // Past this point the chain has given up its buffers: a later failure
    // leaves the new description in place and no buffers (measured:
    // GetBuffer and Present then fail with INVALID_CALL until a
    // ResizeBuffers succeeds).
    chain.desc.buffer_count = count == 0 ? chain.desc.buffer_count : count;
    chain.desc.width = width;
    chain.desc.height = height;
    chain.desc.format = format == 0 ? chain.desc.format : format;
    chain.desc.flags = flags;
    chain.buffers_lost = true;
    // Views that only bindings keep alive die with the old buffers.
    unbind_texture_views(chain.back_buffer, 0, false);
    for (const GuestAddr index : chain.index_buffers) {
        if (index != 0) unbind_texture_views(index, 0, false);
    }
    release_swap_chain_index_buffers(chain);
    // Late failures, measured with the description already updated: a
    // bitblt chain's format outside its set, GDI_COMPATIBLE on a non-BGRA
    // format, and a flip chain's RESTRICT_SHARED_RESOURCE_DRIVER.
    const u32 new_format = chain.desc.format;
    if (!dxgi_bitblt_swap_chain_format(new_format) ||
        ((flags & kSwapChainFlagGdiCompatible) != 0 &&
         !dxgi_gdi_compatible_format(new_format)) ||
        (flip && (flags & kSwapChainFlagRestrictSharedResourceDriver) != 0)) {
        return d3d11::E_INVALIDARG;
    }
    const graphics::PixelFormat pixel_format = backend_format(new_format);
    if (pixel_format == graphics::PixelFormat::Unknown) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::ResizeBuffers: "
            "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM back buffers -> "
            "E_INVALIDARG");
        return d3d11::E_INVALIDARG;
    }
    if (width > kSwapChainMaxDimension || height > kSwapChainMaxDimension) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::ResizeBuffers: back buffer larger than the "
            "16384 Metal texture limit -> E_OUTOFMEMORY");
        return d3d11::E_OUTOFMEMORY;
    }
    auto texture = m_textures.find(chain.back_buffer);
    if (texture == m_textures.end()) return DXGI_ERROR_INVALID_CALL;
    if (chain.backend &&
        !chain.backend->resize(width, height, pixel_format,
                               chain.desc.buffer_count)) {
        d3d11_diag("IDXGISwapChain::ResizeBuffers chain=0x%llx %ux%u "
                   "fmt=%u -> E_OUTOFMEMORY (backend)",
                   static_cast<unsigned long long>(self), width, height,
                   new_format);
        return d3d11::E_OUTOFMEMORY;
    }
    texture->second.desc.width = width;
    texture->second.desc.height = height;
    texture->second.desc.format = new_format;
    // Recreate the guest back-buffer Metal texture to match the new size.
    // Present blits this texture; leaving the old size would leave draws
    // writing a stale buffer while the drawable resized.
    graphics::BackendTexture2DDesc backend_desc = texture->second.backend
        ? texture->second.backend->desc()
        : graphics::BackendTexture2DDesc{};
    backend_desc.width = width;
    backend_desc.height = height;
    backend_desc.pixel_format = pixel_format;
    backend_desc.render_target = true;
    backend_desc.shader_resource =
        (texture->second.desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0;
    backend_desc.unordered_access =
        (texture->second.desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    backend_desc.cpu_writable = false;
    auto replacement = m_backend.create_texture_2d(backend_desc, nullptr, 0);
    if (!replacement || !replacement->valid()) {
        return d3d11::E_OUTOFMEMORY;
    }
    texture->second.backend = std::move(replacement);
    ++m_texture_backend_gen;  // R4 srv-memo: a texture's backend replaced
    if (!create_swap_chain_ring(chain, texture->second)) {
        return d3d11::E_OUTOFMEMORY;
    }
    chain.buffers_lost = false;
    // IDXGISwapChain2 source size follows the new buffers (measured).
    chain.source_width = width;
    chain.source_height = height;
    d3d11_diag(
        "IDXGISwapChain::ResizeBuffers chain=0x%llx -> S_OK "
        "%ux%u fmt=%u buffers=%u flags=0x%x",
        static_cast<unsigned long long>(self), width, height, new_format,
        chain.desc.buffer_count, flags);
    return d3d11::S_OK;
}

} // namespace vitreo::winapi
