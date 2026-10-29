// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain.cpp
/// @brief DXGI swap-chain creation, validation and lifetime: the object
///        and reference helpers, the window queries,
///        validate_swap_chain_desc, create_swap_chain and the release /
///        destroy path. Split out of d3d11.cpp as verbatim code motion.
///        The rest of the swap chain lives in the sibling files
///        d3d11_swapchain_factory.cpp (descriptor marshaling and the
///        factory CreateSwapChain* entry points),
///        d3d11_swapchain_buffers.cpp (buffer ring and ResizeBuffers),
///        d3d11_swapchain_fullscreen.cpp (exclusive full-screen),
///        d3d11_swapchain_backend.cpp (host presenter binding,
///        composition and colour space), d3d11_swapchain_foreign.cpp
///        (cross-process and foreign-window present),
///        d3d11_swapchain_present.cpp (Present / Present1),
///        d3d11_swapchain_timing.cpp (frame latency and frame
///        statistics), d3d11_swapchain_methods.cpp (the IDXGISwapChain
///        vtable) and d3d11_swapchain_media.cpp (the IDXGISwapChainMedia
///        vtable). Their shared constants are in
///        d3d11_swapchain_internal.hpp.
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

bool D3D11Manager::is_swap_chain_object(GuestAddr address) const {
    return m_swap_chains.contains(address);
}

bool D3D11Manager::retain_swap_chain_reference(GuestAddr address) {
    auto swap_chain = m_swap_chains.find(address);
    if (swap_chain == m_swap_chains.end()) return false;
    ++swap_chain->second.ref_count;
    return true;
}

void D3D11Manager::release_swap_chain_reference(GuestAddr address) {
    (void)release_swap_chain(address);
}

std::optional<std::pair<u32, u32>> D3D11Manager::window_client_size(
    u64 hwnd) const {
    if (!m_window_services.client_size || hwnd == 0) return std::nullopt;
    return m_window_services.client_size(hwnd);
}

bool D3D11Manager::window_exists(u64 hwnd) const {
    if (hwnd == 0) return false;
    if (!m_window_services.client_size) return true;
    return m_window_services.client_size(hwnd).has_value();
}

u32 D3D11Manager::validate_swap_chain_desc(const SwapChainDesc& desc,
                                           SwapChainApi api,
                                           u32 stereo) const {
    // Every row below is a line of tools/oracle_probe/
    // dxgi_a11_contracts_probe{64,32}.expected ([swapchain.create*]) or
    // dxgi_a11_swapchain_rules_probe{64,32}.expected, both measured on
    // Windows 11. The HRESULT differs by model: format and sample errors of
    // a bitblt chain are E_INVALIDARG, of a flip chain INVALID_CALL.
    const u32 effect = desc.swap_effect;
    const bool flip = dxgi_flip_effect(effect);
    if (effect != kSwapEffectDiscard && effect != kSwapEffectSequential &&
        !flip) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (api == SwapChainApi::Composition && !flip) {
        return DXGI_ERROR_INVALID_CALL;
    }
    // DXGI_MAX_SWAP_CHAIN_BUFFERS; the flip model needs two (Learn
    // DXGI_SWAP_EFFECT).
    if (desc.buffer_count == 0 ||
        desc.buffer_count > kDxgiMaxSwapChainBuffers ||
        (flip && desc.buffer_count < 2)) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (flip ? !dxgi_flip_swap_chain_format(desc.format)
             : !dxgi_bitblt_swap_chain_format(desc.format)) {
        return flip ? DXGI_ERROR_INVALID_CALL : d3d11::E_INVALIDARG;
    }
    if (flip) {
        if (desc.sample_count != 1 || desc.sample_quality != 0) {
            return DXGI_ERROR_INVALID_CALL;
        }
    } else {
        // A bitblt chain may be multisampled: the quality must be one the
        // device reports for the format (or a standard pattern), exactly as
        // for CreateTexture2D. 3x and 32x (no levels) are E_INVALIDARG.
        if (desc.sample_count == 0) return d3d11::E_INVALIDARG;
        // One sample always has exactly one quality level.
        const u32 levels = desc.sample_count == 1
            ? 1u
            : multisample_quality_levels(desc.format, desc.sample_count);
        const bool standard_pattern =
            desc.sample_count > 1 &&
            (desc.sample_quality == kStandardMultisamplePattern ||
             desc.sample_quality == kCenterMultisamplePattern);
        if (levels == 0 ||
            (desc.sample_quality >= levels && !standard_pattern)) {
            return d3d11::E_INVALIDARG;
        }
    }
    // BufferUsage: CPU access is never allowed; the flip model also refuses
    // SHARED, READ_ONLY and DISCARD_ON_PRESENT.
    if ((desc.buffer_usage & kDxgiCpuAccessField) != 0) {
        return flip ? DXGI_ERROR_INVALID_CALL : d3d11::E_INVALIDARG;
    }
    if (flip && (desc.buffer_usage & (kDxgiUsageShared | kDxgiUsageReadOnly |
                                      kDxgiUsageDiscardOnPresent)) != 0) {
        return DXGI_ERROR_INVALID_CALL;
    }
    // Flags: the per-model accepted set; RESTRICT_SHARED_RESOURCE_DRIVER on a
    // flip chain is E_INVALIDARG, everything else outside the set
    // INVALID_CALL.
    const u32 allowed = flip ? kSwapChainFlagsFlip : kSwapChainFlagsBitblt;
    if ((desc.flags & ~(allowed | kSwapChainFlagRestrictSharedResourceDriver)) !=
        0) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (flip && (desc.flags & kSwapChainFlagRestrictSharedResourceDriver) != 0) {
        return d3d11::E_INVALIDARG;
    }
    if ((desc.flags & kSwapChainFlagGdiCompatible) != 0 &&
        !dxgi_gdi_compatible_format(desc.format)) {
        return d3d11::E_INVALIDARG;
    }
    if (api != SwapChainApi::Legacy) {
        // DXGI_SWAP_CHAIN_DESC1::Scaling. STRETCH everywhere; NONE only on a
        // flip HWND chain; ASPECT_RATIO_STRETCH is for CoreWindow chains.
        const u32 scaling = desc.desc1_scaling;
        const bool scaling_ok =
            scaling == kDxgiScalingStretch ||
            (scaling == kDxgiScalingNone && flip &&
             api == SwapChainApi::Hwnd);
        if (!scaling_ok) return DXGI_ERROR_INVALID_CALL;
        // DXGI_ALPHA_MODE: an HWND chain is opaque (UNSPECIFIED / IGNORE);
        // a composition chain may also be PREMULTIPLIED. STRAIGHT is never
        // accepted for a swap chain.
        const u32 alpha = desc.alpha_mode;
        const bool alpha_ok =
            alpha == kDxgiAlphaUnspecified || alpha == kDxgiAlphaIgnore ||
            (alpha == kDxgiAlphaPremultiplied &&
             api == SwapChainApi::Composition);
        if (!alpha_ok) return DXGI_ERROR_INVALID_CALL;
        if (stereo != 0) {
            // Windows accepts a stereo flip chain on an adapter that has a
            // stereo path (WARP does); Vitreo has no stereo presentation and
            // IsWindowedStereoEnabled reports FALSE.
            DXGI_UNIMPLEMENTED_ONCE(
                "dxgi!CreateSwapChainForHwnd: Stereo = TRUE (no stereo "
                "presentation) -> DXGI_ERROR_INVALID_CALL");
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    return d3d11::S_OK;
}

u32 D3D11Manager::create_swap_chain(
    GuestAddr device_interface, SwapChainDesc desc, SwapChainApi api,
    u32 stereo, GuestAddr factory, GuestAddr restrict_output,
    GuestAddr& created) {
    created = 0;
    InterfaceType interface_type{};
    Device* device = find_device(device_interface, &interface_type);
    if (device == nullptr ||
        (interface_type != InterfaceType::Device &&
         interface_type != InterfaceType::D3D10Device &&
         interface_type != InterfaceType::DxgiDevice)) {
        d3d11_diag("CreateSwapChain reject: bad device iface=0x%llx type=%d",
                   static_cast<unsigned long long>(device_interface),
                   device == nullptr ? -1 : static_cast<int>(interface_type));
        return DXGI_ERROR_INVALID_CALL;
    }
    const bool composition = api == SwapChainApi::Composition;
    const bool window_missing =
        !composition && !window_exists(desc.output_window);
    // Learn DXGI_SWAP_CHAIN_DESC / DESC1: a zero Width or Height takes BOTH
    // from the output window's client area (measured: a 0xH request gets the
    // client width AND height). A window that does not exist has no client
    // area to take them from.
    if (!composition && (desc.width == 0 || desc.height == 0)) {
        const auto client = window_client_size(desc.output_window);
        if (!client) {
            if (window_missing) return DXGI_ERROR_INVALID_CALL;
            DXGI_UNIMPLEMENTED_ONCE(
                "dxgi!CreateSwapChain: zero Width/Height with no window "
                "services to read the client area -> "
                "DXGI_ERROR_INVALID_CALL");
            return DXGI_ERROR_INVALID_CALL;
        }
        desc.width = client->first;
        desc.height = client->second;
    }
    if (const u32 invalid = validate_swap_chain_desc(desc, api, stereo);
        invalid != d3d11::S_OK) {
        d3d11_diag(
            "CreateSwapChain reject 0x%08x: %ux%u fmt=%u cnt=%u eff=%u "
            "samples=%u.%u usage=0x%x flags=0x%x scaling=%u alpha=%u",
            invalid, desc.width, desc.height, desc.format, desc.buffer_count,
            desc.swap_effect, desc.sample_count, desc.sample_quality,
            desc.buffer_usage, desc.flags, desc.desc1_scaling,
            desc.alpha_mode);
        return invalid;
    }
    // R10G10B10_XR_BIAS_A2_UNORM is a legal bitblt format with no Metal
    // equivalent that a render target could use.
    if (backend_format(desc.format) == graphics::PixelFormat::Unknown) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!CreateSwapChain: DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM "
            "back buffers -> E_INVALIDARG");
        return d3d11::E_INVALIDARG;
    }
    // Learn IDXGIFactory2::CreateSwapChainForHwnd: a window has at most one
    // flip-model chain; a second one is E_ACCESSDENIED (measured, also via
    // CreateSwapChain). A bitblt chain beside either is allowed.
    if (!composition && dxgi_flip_effect(desc.swap_effect)) {
        for (const auto& [address, other] : m_swap_chains) {
            (void)address;
            if (!other.composition && other.ref_count != 0 &&
                other.desc.output_window == desc.output_window &&
                dxgi_flip_effect(other.desc.swap_effect)) {
                return d3d11::E_ACCESSDENIED;
            }
        }
    }
    // Learn CreateSwapChain: "If you attempt to create a swap chain in
    // full-screen mode, and full-screen mode is unavailable, the swap chain
    // will be created in windowed mode and DXGI_STATUS_OCCLUDED will be
    // returned." The chain is created windowed and then enters full-screen
    // through SetFullscreenState's own path.
    const bool fullscreen_requested = desc.windowed == 0;
    desc.windowed = 1;
    // The Metal back buffer (and CAMetalLayer drawable) cannot exceed the
    // GPU's texture limit. Windows fails such an allocation with
    // E_OUTOFMEMORY (measured on x86 for ResizeBuffers 20000x20000; WARP x64
    // could allocate it).
    if (desc.width > kSwapChainMaxDimension ||
        desc.height > kSwapChainMaxDimension) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!CreateSwapChain: back buffer larger than the 16384 Metal "
            "texture limit -> E_OUTOFMEMORY");
        return d3d11::E_OUTOFMEMORY;
    }
    if ((!composition && !window_missing && !m_presentation_view_resolver) ||
        !ensure_storage()) {
        d3d11_diag("CreateSwapChain reject: resolver/storage");
        return DXGI_ERROR_INVALID_CALL;
    }
    // The chain's backend is created BEFORE the guest object so no guest
    // address is published for a chain that cannot present.
    const GuestAddr device_owner =
        m_interfaces.find(device_interface)->second.owner;
    const GuestAddr address = allocate_object(m_swap_chain_vtable);
    if (address == 0) return d3d11::E_OUTOFMEMORY;
    // The IDXGISwapChainMedia facet (object + 0x20), as a texture's DXGI
    // facets sit inside its object.
    if (!write_pointer(address + kSwapChainMediaFacet,
                       m_swap_chain_media_vtable)) {
        free_object(address);
        return d3d11::E_OUTOFMEMORY;
    }

    SwapChain swap_chain{};
    swap_chain.interface_address = address;
    swap_chain.device_owner = device_owner;
    swap_chain.desc = desc;
    swap_chain.composition = composition;
    swap_chain.window_missing = window_missing;
    swap_chain.source_width = desc.width;
    swap_chain.source_height = desc.height;
    // The only colour space an FP16 chain presents is linear scRGB
    // (measured), so that is its default; every other format is sRGB.
    swap_chain.color_space = desc.format == kDxgiFormatR16G16B16A16Float
        ? kDxgiColorSpaceRgbFullG10NoneP709
        : kDxgiColorSpaceRgbFullG22NoneP709;
    m_swap_chains.emplace(address, std::move(swap_chain));
    if (!composition && !window_missing &&
        !create_swap_chain_backend(m_swap_chains.at(address))) {
        m_swap_chains.erase(address);
        free_object(address);
        d3d11_diag("CreateSwapChain reject: Metal backend create failed");
        return DXGI_ERROR_INVALID_CALL;
    }
    (void)add_ref(device->device_interface);

    Texture2DDesc back_buffer_desc{};
    back_buffer_desc.width = desc.width;
    back_buffer_desc.height = desc.height;
    back_buffer_desc.format = desc.format;
    back_buffer_desc.sample_count = desc.sample_count;
    back_buffer_desc.sample_quality = desc.sample_quality;
    // DXGI BufferUsage maps to D3D11 bind flags. Callers sample the back
    // buffer, which requires DXGI_USAGE_SHADER_INPUT → SRV;
    // DXGI_USAGE_UNORDERED_ACCESS is a UAV binding.
    back_buffer_desc.bind_flags = D3D11_BIND_RENDER_TARGET;
    if ((desc.buffer_usage & DXGI_USAGE_SHADER_INPUT) != 0) {
        back_buffer_desc.bind_flags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if ((desc.buffer_usage & kDxgiUsageUnorderedAccess) != 0) {
        back_buffer_desc.bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    const GuestAddr back_buffer = create_texture_2d(
        device_owner, back_buffer_desc, nullptr, 0, address);
    const auto fail_after_device_ref = [&](const char* why) {
        auto chain = m_swap_chains.find(address);
        if (chain != m_swap_chains.end()) {
            const GuestAddr texture = chain->second.back_buffer;
            m_swap_chains.erase(chain);
            if (texture != 0) (void)release_texture(texture);
        }
        free_object(address);
        release_device_reference(device_owner);
        d3d11_diag("CreateSwapChain reject: %s", why);
        return d3d11::E_OUTOFMEMORY;
    };
    if (back_buffer == 0) {
        return fail_after_device_ref("back-buffer texture failed");
    }
    auto& created_swap_chain = m_swap_chains.find(address)->second;
    created_swap_chain.back_buffer = back_buffer;
    auto texture = m_textures.find(back_buffer);
    if (texture == m_textures.end() || !texture->second.backend ||
        !create_swap_chain_ring(created_swap_chain, texture->second)) {
        return fail_after_device_ref("buffer ring / resolve target failed");
    }
    if (factory != 0 && m_dxgi.retain_object(factory)) {
        created_swap_chain.factory = factory;
    }
    if (restrict_output != 0 && m_dxgi.retain_object(restrict_output)) {
        created_swap_chain.restrict_output = restrict_output;
    }
    if (window_missing) {
        d3d11_diag("CreateSwapChain hwnd=0x%llx is not a window: the chain "
                   "exists but every Present is occluded",
                   static_cast<unsigned long long>(desc.output_window));
    }
    created = address;
    if (!fullscreen_requested) return d3d11::S_OK;
    return enter_exclusive_fullscreen(address, 0) == d3d11::S_OK
        ? d3d11::S_OK
        : d3d11::DXGI_STATUS_OCCLUDED;
}

void D3D11Manager::destroy_swap_chain_if_unreferenced(GuestAddr address) {
    auto swap_chain = m_swap_chains.find(address);
    if (swap_chain == m_swap_chains.end() ||
        swap_chain->second.ref_count != 0) {
        return;
    }
    // A buffer the guest still holds (GetBuffer(0) or GetBuffer(n)) keeps
    // the chain alive until it is released.
    std::vector<GuestAddr> buffers{swap_chain->second.back_buffer};
    buffers.insert(buffers.end(), swap_chain->second.index_buffers.begin(),
                   swap_chain->second.index_buffers.end());
    for (const GuestAddr buffer : buffers) {
        auto texture = m_textures.find(buffer);
        if (buffer != 0 && texture != m_textures.end() &&
            texture->second.ref_count > 1) {
            return;
        }
    }
    SwapChain dead = std::move(swap_chain->second);
    m_swap_chains.erase(swap_chain);
    free_object(address);
    dead.backend.reset();
    release_swap_chain_index_buffers(dead);
    close_swap_chain_latency_object(dead);
    if (dead.back_buffer != 0) (void)release_texture(dead.back_buffer);
    if (dead.factory != 0) m_dxgi.release_dxgi_object(dead.factory);
    if (dead.restrict_output != 0) {
        m_dxgi.release_dxgi_object(dead.restrict_output);
    }
    release_device_reference(dead.device_owner);
}

u32 D3D11Manager::release_swap_chain(GuestAddr address) {
    auto swap_chain = m_swap_chains.find(address);
    if (swap_chain == m_swap_chains.end() ||
        swap_chain->second.ref_count == 0) {
        return 0;
    }
    // Learn (DXGI Overview, Destroying a Swap Chain) asks applications to
    // leave full-screen before the last Release. One that does not still
    // gets the desktop mode, its window and the host Space back.
    if (swap_chain->second.ref_count == 1 &&
        (swap_chain->second.fullscreen || swap_chain->second.host_space)) {
        leave_exclusive_fullscreen(address, true);
        swap_chain = m_swap_chains.find(address);
        if (swap_chain == m_swap_chains.end() ||
            swap_chain->second.ref_count == 0) {
            return 0;
        }
    }
    const u32 count = --swap_chain->second.ref_count;
    if (count == 0) destroy_swap_chain_if_unreferenced(address);
    return count;
}

} // namespace vitreo::winapi
