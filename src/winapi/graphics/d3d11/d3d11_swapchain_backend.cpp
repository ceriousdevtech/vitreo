// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_backend.cpp
/// @brief Binding a DXGI swap chain to the host presenter: the
///        composition layer, window presentation refresh, backend
///        creation, presentation state and the colour-space contract.
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

bool D3D11Manager::apply_composition_layer(SwapChain& chain) {
    if (!chain.composition || !chain.composition_layer || !chain.backend) {
        return false;
    }
    // IDXGISwapChain2::SetMatrixTransform scales/translates the chain's
    // content inside its visual: it applies before the visual's own
    // transform. Both are Direct2D row-vector 3x2 matrices (m11 m12 m21 m22
    // dx dy), so the combined transform is chain x visual.
    graphics::BackendCompositionLayerDesc layer = *chain.composition_layer;
    const auto& a = chain.matrix_transform;
    const auto b = layer.transform;
    layer.transform = {
        a[0] * b[0] + a[1] * b[2],
        a[0] * b[1] + a[1] * b[3],
        a[2] * b[0] + a[3] * b[2],
        a[2] * b[1] + a[3] * b[3],
        a[4] * b[0] + a[5] * b[2] + b[4],
        a[4] * b[1] + a[5] * b[3] + b[5],
    };
    return chain.backend->configure_composition_layer(layer);
}

bool D3D11Manager::bind_composition_swap_chain(GuestAddr address,
    GuestAddr hwnd,
    const graphics::BackendCompositionLayerDesc& layer) {
    auto swap_chain = m_swap_chains.find(address);
    if (swap_chain == m_swap_chains.end() ||
        !swap_chain->second.composition || hwnd == 0) {
        return false;
    }
    if (swap_chain->second.desc.output_window != hwnd) {
        swap_chain->second.backend.reset();
        swap_chain->second.desc.output_window = hwnd;
    }
    swap_chain->second.composition_layer =
        std::make_shared<graphics::BackendCompositionLayerDesc>(layer);
    bool bound = create_swap_chain_backend(swap_chain->second) &&
                 apply_composition_layer(swap_chain->second);
    if (bound) {
        const auto& t = layer.transform;
        const auto& own = swap_chain->second.matrix_transform;
        const bool identity = t[0] == 1.0f && t[1] == 0.0f && t[2] == 0.0f &&
                              t[3] == 1.0f && t[4] == 0.0f && t[5] == 0.0f &&
                              own[0] == 1.0f && own[3] == 1.0f &&
                              own[4] == 0.0f && own[5] == 0.0f;
        auto& clip = swap_chain->second.composition_clip;
        clip.valid = layer.has_clip_rect && identity;
        clip.left = layer.clip_rect[0];
        clip.top = layer.clip_rect[1];
        clip.right = layer.clip_rect[2];
        clip.bottom = layer.clip_rect[3];
    }
    if (bound && swap_chain->second.pending_present) {
        bound = present_swap_chain_frame(
            address, swap_chain->second.pending_sync_interval,
            swap_chain->second.pending_present_flags, true);
        if (bound) {
            swap_chain->second.pending_present = false;
            d3d11_diag(
                "Flush deferred composition present chain=0x%llx -> ok",
                static_cast<unsigned long long>(address));
        }
    }
    // The cross-process foreign-surface path carries only a size, so record
    // exactly what DirectComposition geometry a bind computes here. That is
    // the difference between the two composition paths, and it decides
    // whether the boundary is losing anything that matters.
    d3d11_diag("Bind composition swap chain=0x%llx hwnd=0x%llx "
               "xform=[%g %g %g %g %g %g] clip=%d[%g %g %g %g] z=%d -> %s",
               static_cast<unsigned long long>(address),
               static_cast<unsigned long long>(hwnd),
               layer.transform[0], layer.transform[1], layer.transform[2],
               layer.transform[3], layer.transform[4], layer.transform[5],
               layer.has_clip_rect ? 1 : 0,
               layer.clip_rect[0], layer.clip_rect[1],
               layer.clip_rect[2], layer.clip_rect[3],
               layer.z_order,
               bound ? "ok" : "failed");
    return bound;
}

bool D3D11Manager::refresh_window_presentation(GuestAddr hwnd) {
    bool matched = false;
    bool refreshed = false;
    for (auto& [address, swap_chain] : m_swap_chains) {
        if (swap_chain.desc.output_window != hwnd || !swap_chain.backend) {
            continue;
        }
        matched = true;
        refreshed = present_swap_chain_frame(
            address, swap_chain.pending_sync_interval,
            swap_chain.pending_present_flags, true) || refreshed;
    }
    d3d11_diag("Refresh window presentation hwnd=0x%llx matched=%d -> %s",
               static_cast<unsigned long long>(hwnd), matched,
               refreshed ? "ok" : "failed");
    return refreshed;
}

bool D3D11Manager::create_swap_chain_backend(SwapChain& swap_chain) {
    if (swap_chain.backend) return swap_chain.backend->valid();
    if (!m_presentation_view_resolver ||
        swap_chain.desc.output_window == 0) {
        return false;
    }
    void* native_view =
        m_presentation_view_resolver(swap_chain.desc.output_window);
    if (native_view == nullptr) {
        d3d11_diag(
            "CreateSwapChain backend: presentation view null hwnd=0x%llx",
            static_cast<unsigned long long>(swap_chain.desc.output_window));
        return false;
    }
    graphics::BackendSwapChainDesc backend_desc{};
    backend_desc.native_view = native_view;
    backend_desc.width = swap_chain.desc.width;
    backend_desc.height = swap_chain.desc.height;
    backend_desc.pixel_format = backend_format(swap_chain.desc.format);
    backend_desc.buffer_count = swap_chain.desc.buffer_count;
    backend_desc.vsync = true;
    // DXGI_ALPHA_MODE_PREMULTIPLIED=1, STRAIGHT=2.
    backend_desc.premultiplied_alpha =
        swap_chain.desc.alpha_mode == 1u || swap_chain.desc.alpha_mode == 2u;
    swap_chain.backend = m_backend.create_swap_chain(backend_desc);
    if (!swap_chain.backend || !swap_chain.backend->valid()) return false;
    apply_swap_chain_presentation_state(swap_chain);
    return true;
}

void D3D11Manager::apply_swap_chain_presentation_state(SwapChain& chain) {
    if (!chain.backend) return;
    // The colour space, background, rotation and HDR metadata the guest set
    // (or the defaults) belong to the chain, not to one backend instance.
    if (!chain.backend->set_color_space(
            swap_chain_backend_color_space(chain.color_space))) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain3::SetColorSpace1: the presenter cannot tag "
            "this colour space; the image is shown as sRGB");
    }
    // The default (opaque black) is the presenter's own plate; a
    // transparent composition chain must keep its clear one.
    const bool default_background =
        chain.background_color == std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
    if (!default_background &&
        !chain.backend->set_background_color(chain.background_color)) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain1::SetBackgroundColor: the presenter keeps "
            "its own background");
    }
    if (!chain.backend->set_rotation(chain.rotation - 1)) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain1::SetRotation: the presenter does not "
            "rotate the image; it is shown unrotated");
    }
    if (chain.hdr_metadata_type == kDxgiHdrMetadataHdr10) {
        (void)chain.backend->set_hdr10_metadata(chain.hdr_metadata);
    }
}

bool D3D11Manager::swap_chain_color_space_supported(const SwapChain& chain,
                                                    u32 color_space) const {
    // Measured (dxgi_a11_swapchain_rules_probe [colorspace]): an FP16 chain
    // takes only linear scRGB (G10_NONE_P709); an 8/10-bit bitblt chain only
    // sRGB (G22_NONE_P709); an 8/10-bit flip chain also HDR10 (G2084 P2020),
    // full-range G22 P2020, and the studio-range variants. Vitreo answers
    // only for what the presenter shows correctly: studio (limited) range
    // needs an expansion the CAMetalLayer path does not have.
    const bool fp16 = chain.desc.format == kDxgiFormatR16G16B16A16Float;
    if (fp16) return color_space == kDxgiColorSpaceRgbFullG10NoneP709;
    if (color_space == kDxgiColorSpaceRgbFullG22NoneP709) return true;
    if (!dxgi_flip_effect(chain.desc.swap_effect)) return false;
    return color_space == kDxgiColorSpaceRgbFullG2084NoneP2020 ||
           color_space == kDxgiColorSpaceRgbFullG22NoneP2020;
}

graphics::BackendColorSpace D3D11Manager::swap_chain_backend_color_space(
    u32 color_space) {
    switch (color_space) {
    case kDxgiColorSpaceRgbFullG10NoneP709:
        return graphics::BackendColorSpace::ExtendedLinearSrgb;
    case kDxgiColorSpaceRgbFullG2084NoneP2020:
        return graphics::BackendColorSpace::Bt2100Pq;
    case kDxgiColorSpaceRgbFullG22NoneP2020:
        return graphics::BackendColorSpace::Bt2020SrgbGamma;
    default:
        return graphics::BackendColorSpace::Srgb;
    }
}

} // namespace vitreo::winapi
