// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_fullscreen.cpp
/// @brief DXGI swap-chain exclusive full-screen for the D3D11 chains:
///        IDXGISwapChain::SetFullscreenState and ResizeTarget, on the state
///        machine shared with the D3D12 chains
///        (winapi/graphics/dxgi/dxgi_exclusive_fullscreen.hpp).
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_swapchain_internal.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/dxgi/dxgi_exclusive_fullscreen.hpp"

namespace vitreo::winapi {

// =============================================================================
// Exclusive full-screen (IDXGISwapChain::SetFullscreenState / ResizeTarget)
//
// The guest display mode goes through the owner DirectDraw and D3D9 exclusive
// mode share (user32's virtual display mode; WindowServices::enter_exclusive)
// and the host presents it as a macOS Space (WindowServices::host_fullscreen),
// never as a covering window (.docs/windowing-native-presentation.md rules 3
// and 6).
// =============================================================================

namespace {

dxgi::ExclusiveWindowServices exclusive_services(
    const D3D11Manager::WindowServices& window) {
    dxgi::ExclusiveWindowServices services;
    services.client_size = window.client_size;
    services.window_rect = window.window_rect;
    services.enter_exclusive = window.enter_exclusive;
    services.leave_exclusive = window.leave_exclusive;
    services.host_fullscreen = window.host_fullscreen;
    services.application_active = window.application_active;
    services.window_occluded = window.window_occluded;
    services.has_keyboard_focus = window.has_keyboard_focus;
    return services;
}

}  // namespace

u32 D3D11Manager::enter_exclusive_fullscreen(GuestAddr self, GuestAddr target) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    SwapChain& chain = found->second;
    // Learn: not on a CreateSwapChainForComposition chain; a D3D11
    // FRAME_LATENCY_WAITABLE_OBJECT chain "isn't supported in full-screen
    // mode" (DXGI_SWAP_CHAIN_FLAG: "unless the render API is Direct3D 12";
    // measured a73 on the RTX: INVALID_CALL for TRUE and FALSE on D3D11,
    // S_OK on D3D12).
    if (chain.composition ||
        (chain.desc.flags & kSwapChainFlagFrameLatencyWaitableObject) != 0) {
        return DXGI_ERROR_INVALID_CALL;
    }
    auto device = m_devices.find(chain.device_owner);
    if (device == m_devices.end()) return DXGI_ERROR_INVALID_CALL;
    dxgi::ExclusiveRequest request;
    request.hwnd = chain.desc.output_window;
    request.adapter = device->second.adapter;
    request.buffer_width = chain.desc.width;
    request.buffer_height = chain.desc.height;
    request.flags = chain.desc.flags;
    request.flip_model = dxgi_flip_effect(chain.desc.swap_effect);
    request.window_missing = chain.window_missing;
    const u32 result = dxgi::enter_exclusive(
        m_dxgi, exclusive_services(m_window_services), self, request, target,
        [this, self]() -> dxgi::ExclusiveState* {
            auto it = m_swap_chains.find(self);
            return it == m_swap_chains.end() ? nullptr : &it->second.exclusive;
        },
        [this](const std::function<void()>& transition) {
            DxgiManager::ApiUnlock unlocked(m_dxgi);
            transition();
        },
        &d3d11_diag);
    if (result == d3d11::S_OK) {
        if (auto it = m_swap_chains.find(self); it != m_swap_chains.end()) {
            it->second.desc.windowed = 0;
        }
    }
    return result;
}

void D3D11Manager::leave_exclusive_fullscreen(GuestAddr self,
                                              bool leave_host_space) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return;
    found->second.desc.windowed = 1;
    dxgi::leave_exclusive(
        m_dxgi, exclusive_services(m_window_services), self,
        found->second.desc.output_window,
        dxgi_flip_effect(found->second.desc.swap_effect),
        [this, self]() -> dxgi::ExclusiveState* {
            auto it = m_swap_chains.find(self);
            return it == m_swap_chains.end() ? nullptr : &it->second.exclusive;
        },
        leave_host_space,
        [this](const std::function<void()>& transition) {
            DxgiManager::ApiUnlock unlocked(m_dxgi);
            transition();
        },
        &d3d11_diag);
}

u32 D3D11Manager::resize_exclusive_target(GuestAddr self, u32 width,
                                          u32 height) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    return dxgi::resize_exclusive_target(
        m_dxgi, exclusive_services(m_window_services), self,
        found->second.desc.output_window, found->second.desc.flags, width,
        height,
        [this, self]() -> dxgi::ExclusiveState* {
            auto it = m_swap_chains.find(self);
            return it == m_swap_chains.end() ? nullptr : &it->second.exclusive;
        },
        [this](const std::function<void()>& transition) {
            DxgiManager::ApiUnlock unlocked(m_dxgi);
            transition();
        },
        &d3d11_diag);
}

void D3D11Manager::relinquish_fullscreen_if_inactive(GuestAddr self) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return;
    SwapChain& chain = found->second;
    const auto& active = m_window_services.application_active;
    if (!active) return;
    if (chain.exclusive.occluded_by_focus_loss && active()) {
        chain.exclusive.occluded_by_focus_loss = false;
    }
    if (!chain.exclusive.fullscreen || active()) return;
    // DXGI may change the display state "in response to end user or system
    // requests" (Learn SetFullscreenState): another application becoming
    // foreground relinquishes full-screen. The host Space stays, as for any
    // Mac full-screen app the user Cmd-Tabs away from. (Not measured for
    // D3D11; the D3D12 chains keep full-screen and are occluded, a73
    // focus12.) A relinquish is not the application's transition, so it does
    // not ask for ResizeBuffers.
    const bool needed = chain.exclusive.needs_resize_buffers;
    leave_exclusive_fullscreen(self, false);
    found = m_swap_chains.find(self);
    if (found != m_swap_chains.end()) {
        found->second.exclusive.occluded_by_focus_loss = true;
        found->second.exclusive.needs_resize_buffers = needed;
    }
}

} // namespace vitreo::winapi
