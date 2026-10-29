// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_fullscreen.cpp
/// @brief DXGI swap-chain exclusive full-screen:
///        IDXGISwapChain::SetFullscreenState and ResizeTarget.
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

// =============================================================================
// Exclusive full-screen (IDXGISwapChain::SetFullscreenState / ResizeTarget)
//
// Sources: Learn IDXGISwapChain::SetFullscreenState, GetFullscreenState,
// ResizeTarget; "DXGI Overview" (Care and Feeding of the Swap Chain, Choosing
// the DXGI Output and Size, Switching Modes); "DXGI Best Practices"
// (Full-Screen Issues); DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH.
//
// The guest display mode goes through the owner DirectDraw and D3D9 exclusive
// mode share (user32's virtual display mode; WindowServices::enter_exclusive)
// and the host presents it as a macOS Space (WindowServices::host_fullscreen),
// never as a covering window (.docs/windowing-native-presentation.md rules 3
// and 6).
// =============================================================================

u32 D3D11Manager::enter_exclusive_fullscreen(GuestAddr self, GuestAddr target) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    if (m_fullscreen_transition) return d3d11::DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    const WindowServices& window = m_window_services;
    if (!window.enter_exclusive || !window.leave_exclusive ||
        !window.host_fullscreen || !window.has_keyboard_focus ||
        !window.window_rect) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::SetFullscreenState: no window system "
            "attached -> DXGI_ERROR_NOT_CURRENTLY_AVAILABLE");
        return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    // A non-NULL pTarget is "the output target that contains the swap
    // chain": an IDXGIOutput. Every output Vitreo enumerates belongs to its
    // one display adapter, the adapter every device is created on.
    if (target != 0 && !m_dxgi.is_output_object(target)) {
        return DXGI_ERROR_INVALID_CALL;
    }
    SwapChain& chain = found->second;
    // Learn: not on a CreateSwapChainForComposition chain; a
    // FRAME_LATENCY_WAITABLE_OBJECT chain "isn't supported in full-screen
    // mode" (measured: INVALID_CALL).
    if (chain.composition ||
        (chain.desc.flags & kSwapChainFlagFrameLatencyWaitableObject) != 0) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const u64 hwnd = chain.desc.output_window;
    // Already full-screen on the output DXGI would choose: nothing changes.
    if (chain.fullscreen && target == 0) return d3d11::S_OK;
    // Learn: "There are many reasons why a windowed-mode swap chain cannot
    // switch to full-screen mode": the output window is occluded (here, an
    // HWND that is not a window), or "does not have keyboard focus".
    if (chain.window_missing || !window.has_keyboard_focus(hwnd)) {
        return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    // "Another application is already in full-screen mode": the guest has
    // one desktop, so one chain of this process holds its display mode.
    if (m_exclusive_chain != 0 && m_exclusive_chain != self &&
        m_swap_chains.contains(m_exclusive_chain)) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::SetFullscreenState: a second exclusive "
            "full-screen chain in one process -> "
            "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE");
        return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    auto device = m_devices.find(chain.device_owner);
    if (device == m_devices.end()) return DXGI_ERROR_INVALID_CALL;
    const GuestAddr adapter = device->second.adapter;

    // The output: pTarget, else (Learn, "Choosing the DXGI Output and Size")
    // the one containing most of the window.
    std::optional<dxgi::OutputInfo> info;
    if (target != 0) {
        info = m_dxgi.output_info_of(target);
    } else if (chain.fullscreen && chain.fullscreen_output != 0) {
        info = m_dxgi.output_info_of(chain.fullscreen_output);
    } else if (const auto rect = window.window_rect(hwnd)) {
        const GuestAddr containing = m_dxgi.create_containing_output(
            adapter, (*rect)[0], (*rect)[1], (*rect)[2], (*rect)[3]);
        if (containing != 0) {
            info = m_dxgi.output_info_of(containing);
            m_dxgi.release_dxgi_object(containing);
        }
    }
    // Measured (dxgi_exclusive_fullscreen_probe, an adapter without outputs):
    // NOT_CURRENTLY_AVAILABLE.
    if (!info) return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    if (found->second.fullscreen &&
        found->second.fullscreen_output != 0) {
        const auto current =
            m_dxgi.output_info_of(found->second.fullscreen_output);
        if (current && current->device_name == info->device_name) {
            return d3d11::S_OK;
        }
    }

    // The mode. Learn "Switching Modes": DXGI changes the display mode only
    // with DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; "the swap chain makes an
    // implicit promise that if you choose a back buffer that exactly matches
    // a display mode supported by the target output, then it will switch to
    // that display mode". DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH: otherwise
    // "the display mode ... will be changed to match the dimensions of the
    // application window". Without the flag, "DXGI defaults to the desktop
    // resolution for full-screen swap chains" (Best Practices).
    const auto [native_width, native_height] = dxgi::output_native_size(*info);
    if (native_width == 0 || native_height == 0) {
        return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    const SwapChainDesc desc = found->second.desc;
    VirtualDisplayMode mode{native_width, native_height};
    if ((desc.flags & kSwapChainFlagAllowModeSwitch) != 0) {
        bool exact = false;
        for (const auto listed : virtual_display_modes(native_width,
                                                       native_height)) {
            exact = exact || (listed.width == desc.width &&
                              listed.height == desc.height);
        }
        if (exact) {
            mode = {desc.width, desc.height};
        } else {
            const auto client = window_client_size(hwnd);
            const u32 want_w =
                client && client->first != 0 ? client->first : desc.width;
            const u32 want_h =
                client && client->second != 0 ? client->second : desc.height;
            mode = closest_virtual_display_mode(native_width, native_height,
                                                want_w, want_h);
        }
    }
    const bool change_mode =
        mode.width != native_width || mode.height != native_height;
    // The virtual display mode is the primary display's (as for
    // ChangeDisplaySettings, which DISP_CHANGE_FAILs another device).
    if (change_mode && (info->left != 0 || info->top != 0)) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::SetFullscreenState: a mode switch on a "
            "secondary output -> DXGI_ERROR_NOT_CURRENTLY_AVAILABLE");
        return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    const std::array<i32, 4> output_rect{
        info->left, info->top,
        info->left + static_cast<i32>(native_width),
        info->top + static_cast<i32>(native_height)};
    // The window rectangle to restore; kept across a re-entry.
    if (!found->second.fullscreen) {
        found->second.windowed_rect = window.window_rect(hwnd);
    }
    const std::string device_name = info->device_name;

    // The transition sends the window messages (WM_DISPLAYCHANGE, WM_SIZE)
    // the application handles, possibly on another thread; DXGI's lock is
    // not held across them (Learn, Multithreading and DXGI).
    bool entered = false;
    {
        const auto enter = window.enter_exclusive;
        const auto host = window.host_fullscreen;
        m_fullscreen_transition = true;
        DxgiManager::ApiUnlock unlocked(m_dxgi);
        // The host Space first: a window that already holds a borderless
        // full-screen Space (desktop-sized popup) is taken over by exclusive
        // mode before the mode change resizes it off the monitor rectangle.
        host(hwnd, true);
        entered = enter(hwnd, output_rect, mode.width, mode.height,
                        change_mode);
        if (!entered) host(hwnd, false);
    }
    m_fullscreen_transition = false;
    found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    if (!entered) {
        found->second.windowed_rect.reset();
        return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    SwapChain& entered_chain = found->second;
    // "When changing to full-screen mode, DXGI creates a new output object"
    // (Best Practices, Gamma and DXGI): GetFullscreenState returns it.
    const GuestAddr output = m_dxgi.create_output_named(adapter, device_name);
    if (entered_chain.fullscreen_output != 0) {
        m_dxgi.release_dxgi_object(entered_chain.fullscreen_output);
    }
    entered_chain.fullscreen_output = output;
    entered_chain.fullscreen = true;
    entered_chain.desc.windowed = 0;
    entered_chain.mode_width = mode.width;
    entered_chain.mode_height = mode.height;
    entered_chain.mode_changed = change_mode;
    entered_chain.host_space = true;
    entered_chain.occluded_by_focus_loss = false;
    m_exclusive_chain = self;
    d3d11_diag("IDXGISwapChain::SetFullscreenState(TRUE) chain=0x%llx "
               "hwnd=0x%llx mode=%ux%u%s",
               static_cast<unsigned long long>(self),
               static_cast<unsigned long long>(hwnd), mode.width, mode.height,
               change_mode ? " (switched)" : " (desktop)");
    return d3d11::S_OK;
}

void D3D11Manager::leave_exclusive_fullscreen(GuestAddr self,
                                              bool leave_host_space) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return;
    SwapChain& chain = found->second;
    const u64 hwnd = chain.desc.output_window;
    const bool was_fullscreen = chain.fullscreen;
    const bool mode_changed = chain.mode_changed;
    const auto restore = chain.windowed_rect;
    const bool leave_space = leave_host_space && chain.host_space;
    const GuestAddr output = chain.fullscreen_output;
    chain.fullscreen = false;
    chain.desc.windowed = 1;
    chain.fullscreen_output = 0;
    chain.mode_width = 0;
    chain.mode_height = 0;
    chain.mode_changed = false;
    chain.windowed_rect.reset();
    if (leave_space) chain.host_space = false;
    if (m_exclusive_chain == self) m_exclusive_chain = 0;
    if (output != 0) m_dxgi.release_dxgi_object(output);
    if (!was_fullscreen && !leave_space) return;
    const auto host = m_window_services.host_fullscreen;
    const auto leave = m_window_services.leave_exclusive;
    m_fullscreen_transition = true;
    {
        DxgiManager::ApiUnlock unlocked(m_dxgi);
        if (leave_space && host) host(hwnd, false);
        if (was_fullscreen && leave) leave(hwnd, mode_changed, restore);
    }
    m_fullscreen_transition = false;
    d3d11_diag("IDXGISwapChain left full-screen chain=0x%llx hwnd=0x%llx "
               "space=%d",
               static_cast<unsigned long long>(self),
               static_cast<unsigned long long>(hwnd), leave_space ? 1 : 0);
}

u32 D3D11Manager::resize_exclusive_target(GuestAddr self, u32 width,
                                          u32 height) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    SwapChain& chain = found->second;
    // Without DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH the full-screen
    // resolution "remains the desktop resolution" (Best Practices); the call
    // itself succeeds.
    if ((chain.desc.flags & kSwapChainFlagAllowModeSwitch) == 0) {
        d3d11_diag("IDXGISwapChain::ResizeTarget chain=0x%llx full-screen "
                   "without ALLOW_MODE_SWITCH: the desktop mode stays",
                   static_cast<unsigned long long>(self));
        return d3d11::S_OK;
    }
    const auto info = m_dxgi.output_info_of(chain.fullscreen_output);
    if (!info) return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    const auto [native_width, native_height] = dxgi::output_native_size(*info);
    // "Changes the display mode on the target output when the swap chain is
    // in full-screen mode": the supported mode closest to the request. A
    // request for the current mode "amounts to a no-operation".
    const VirtualDisplayMode mode =
        width == 0 && height == 0
            ? VirtualDisplayMode{chain.mode_width, chain.mode_height}
            : closest_virtual_display_mode(native_width, native_height, width,
                                           height);
    if (mode.width == chain.mode_width && mode.height == chain.mode_height) {
        return d3d11::S_OK;
    }
    const bool change_mode =
        mode.width != native_width || mode.height != native_height;
    const u64 hwnd = chain.desc.output_window;
    const std::array<i32, 4> output_rect{
        0, 0, static_cast<i32>(native_width), static_cast<i32>(native_height)};
    bool entered = false;
    {
        const auto enter = m_window_services.enter_exclusive;
        if (!enter) return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        m_fullscreen_transition = true;
        DxgiManager::ApiUnlock unlocked(m_dxgi);
        entered = enter(hwnd, output_rect, mode.width, mode.height,
                        change_mode);
    }
    m_fullscreen_transition = false;
    found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return DXGI_ERROR_INVALID_CALL;
    if (!entered) return d3d11::DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    found->second.mode_width = mode.width;
    found->second.mode_height = mode.height;
    found->second.mode_changed = change_mode;
    d3d11_diag("IDXGISwapChain::ResizeTarget chain=0x%llx full-screen mode "
               "-> %ux%u", static_cast<unsigned long long>(self), mode.width,
               mode.height);
    return d3d11::S_OK;
}

void D3D11Manager::relinquish_fullscreen_if_inactive(GuestAddr self) {
    auto found = m_swap_chains.find(self);
    if (found == m_swap_chains.end()) return;
    SwapChain& chain = found->second;
    const auto& active = m_window_services.application_active;
    if (!active) return;
    if (chain.occluded_by_focus_loss && active()) {
        chain.occluded_by_focus_loss = false;
    }
    if (!chain.fullscreen || active()) return;
    // DXGI may change the display state "in response to end user or system
    // requests" (Learn SetFullscreenState): another application becoming
    // foreground relinquishes full-screen. The host Space stays, as for any
    // Mac full-screen app the user Cmd-Tabs away from.
    leave_exclusive_fullscreen(self, false);
    found = m_swap_chains.find(self);
    if (found != m_swap_chains.end()) {
        found->second.occluded_by_focus_loss = true;
    }
}

} // namespace vitreo::winapi
