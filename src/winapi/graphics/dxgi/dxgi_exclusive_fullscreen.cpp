// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_exclusive_fullscreen.cpp
/// @brief DXGI swap-chain exclusive full-screen shared by the D3D11 and
///        D3D12 swap chains (see the header for the sources). Moved out of
///        d3d11_swapchain_fullscreen.cpp so both frontends run one state
///        machine.

#include "winapi/graphics/dxgi/dxgi_exclusive_fullscreen.hpp"

#include "winapi/common/virtual_display_modes.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include <cstdio>
#include <mutex>
#include <string>

namespace vitreo::winapi::dxgi {
namespace {

// winerror.h.
constexpr u32 kSOk = 0x00000000u;
constexpr u32 kInvalidCall = 0x887A0001u;
constexpr u32 kNotCurrentlyAvailable = 0x887A0022u;
constexpr u32 kModeChangeInProgress = 0x087A0008u;
// DXGI_SWAP_CHAIN_FLAG (dxgi.h).
constexpr u32 kFlagAllowModeSwitch = 0x2u;

// One stderr line per call site, the first time it is reached.
#define DXGI_FS_UNIMPLEMENTED_ONCE(text)                                     \
    do {                                                                     \
        static std::once_flag dxgi_fs_unimplemented_once_;                   \
        std::call_once(dxgi_fs_unimplemented_once_, [] {                     \
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED " text "\n");       \
        });                                                                  \
    } while (0)

}  // namespace

u32 enter_exclusive(DxgiManager& dxgi, const ExclusiveWindowServices& window,
                    GuestAddr self, const ExclusiveRequest& request,
                    GuestAddr target, const StateLookup& lookup,
                    const UnlockedScope& unlocked, DiagFn diag) {
    ExclusiveRegistry& registry = dxgi.exclusive_fullscreen();
    if (registry.transition) return kModeChangeInProgress;
    if (!window.enter_exclusive || !window.leave_exclusive ||
        !window.host_fullscreen || !window.has_keyboard_focus ||
        !window.window_rect) {
        DXGI_FS_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::SetFullscreenState: no window system "
            "attached -> DXGI_ERROR_NOT_CURRENTLY_AVAILABLE");
        return kNotCurrentlyAvailable;
    }
    // A non-NULL pTarget is "the output target that contains the swap
    // chain": an IDXGIOutput. Every output Vitreo enumerates belongs to its
    // one display adapter, the adapter every device is created on.
    if (target != 0 && !dxgi.is_output_object(target)) return kInvalidCall;
    ExclusiveState* state = lookup();
    if (state == nullptr) return kInvalidCall;
    const u64 hwnd = request.hwnd;
    // Already full-screen on the output DXGI would choose: nothing changes
    // (measured a73 "TRUE again": S_OK, also from the background).
    if (state->fullscreen && (target == 0 || target == state->output)) {
        return kSOk;
    }
    // Learn: "There are many reasons why a windowed-mode swap chain cannot
    // switch to full-screen mode": the output window is occluded (here, an
    // HWND that is not a window), or "does not have keyboard focus"
    // (measured a73: a window that is not foreground,
    // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE).
    if (!state->fullscreen &&
        (request.window_missing || !window.has_keyboard_focus(hwnd))) {
        return kNotCurrentlyAvailable;
    }
    // "Another application is already in full-screen mode": the guest has
    // one desktop, so one chain of this process holds its display mode.
    if (const GuestAddr owner = registry.owner.load(); owner != 0 && owner != self) {
        DXGI_FS_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::SetFullscreenState: a second exclusive "
            "full-screen chain in one process -> "
            "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE");
        return kNotCurrentlyAvailable;
    }

    // The output: pTarget, else (Learn, "Choosing the DXGI Output and Size")
    // the one containing most of the window.
    std::optional<OutputInfo> info;
    if (target != 0) {
        info = dxgi.output_info_of(target);
    } else if (state->fullscreen && state->output != 0) {
        info = dxgi.output_info_of(state->output);
    } else if (const auto rect = window.window_rect(hwnd)) {
        const GuestAddr containing = dxgi.create_containing_output(
            request.adapter, (*rect)[0], (*rect)[1], (*rect)[2], (*rect)[3]);
        if (containing != 0) {
            info = dxgi.output_info_of(containing);
            dxgi.release_dxgi_object(containing);
        }
    }
    // Measured (a73 WARP, an adapter without outputs): NOT_CURRENTLY_AVAILABLE.
    if (!info) return kNotCurrentlyAvailable;
    if (state->fullscreen && state->output != 0) {
        const auto current = dxgi.output_info_of(state->output);
        if (current && current->device_name == info->device_name) {
            if (target != 0 && target != state->output &&
                dxgi.retain_object(target)) {
                dxgi.release_dxgi_object(state->output);
                state->output = target;
            }
            return kSOk;
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
    const auto [native_width, native_height] = output_native_size(*info);
    if (native_width == 0 || native_height == 0) return kNotCurrentlyAvailable;
    VirtualDisplayMode mode{native_width, native_height};
    if ((request.flags & kFlagAllowModeSwitch) != 0) {
        bool exact = false;
        for (const auto listed :
             virtual_display_modes(native_width, native_height)) {
            exact = exact || (listed.width == request.buffer_width &&
                              listed.height == request.buffer_height);
        }
        if (exact) {
            mode = {request.buffer_width, request.buffer_height};
        } else {
            const auto client = window.client_size
                                    ? window.client_size(hwnd)
                                    : std::nullopt;
            const u32 want_w = client && client->first != 0
                                   ? client->first
                                   : request.buffer_width;
            const u32 want_h = client && client->second != 0
                                   ? client->second
                                   : request.buffer_height;
            mode = closest_virtual_display_mode(native_width, native_height,
                                                want_w, want_h);
        }
    }
    const bool change_mode =
        mode.width != native_width || mode.height != native_height;
    // The virtual display mode is the primary display's (as for
    // ChangeDisplaySettings, which DISP_CHANGE_FAILs another device).
    if (change_mode && (info->left != 0 || info->top != 0)) {
        DXGI_FS_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain::SetFullscreenState: a mode switch on a "
            "secondary output -> DXGI_ERROR_NOT_CURRENTLY_AVAILABLE");
        return kNotCurrentlyAvailable;
    }
    const std::array<i32, 4> output_rect{
        info->left, info->top,
        info->left + static_cast<i32>(native_width),
        info->top + static_cast<i32>(native_height)};
    const bool was_fullscreen = state->fullscreen;
    // The window rectangle to restore; kept across a re-entry.
    if (!state->fullscreen) state->windowed_rect = window.window_rect(hwnd);
    const std::string device_name = info->device_name;

    bool entered = false;
    {
        const auto enter = window.enter_exclusive;
        const auto host = window.host_fullscreen;
        registry.transition = true;
        unlocked([&] {
            // The host Space first: a window that already holds a borderless
            // full-screen Space (desktop-sized popup) is taken over by
            // exclusive mode before the mode change resizes it off the
            // monitor rectangle.
            host(hwnd, true);
            entered = enter(hwnd, output_rect, mode.width, mode.height,
                            change_mode);
            if (!entered) host(hwnd, false);
        });
        registry.transition = false;
    }
    state = lookup();
    if (state == nullptr) return kInvalidCall;
    if (!entered) {
        state->windowed_rect.reset();
        return kNotCurrentlyAvailable;
    }
    // GetFullscreenState returns pTarget itself when one was given (measured
    // a73, D3D11 and D3D12: "passed=1"); without one "DXGI creates a new
    // output object" (Best Practices, Gamma and DXGI).
    GuestAddr output = 0;
    if (target != 0 && dxgi.retain_object(target)) {
        output = target;
    } else {
        output = dxgi.create_output_named(request.adapter, device_name);
    }
    if (state->output != 0) dxgi.release_dxgi_object(state->output);
    state->output = output;
    state->fullscreen = true;
    state->mode_width = mode.width;
    state->mode_height = mode.height;
    state->mode_changed = change_mode;
    state->host_space = true;
    state->occluded_by_focus_loss = false;
    if (request.flip_model && !was_fullscreen) {
        state->needs_resize_buffers = !state->needs_resize_buffers;
    }
    registry.owner = self;
    if (diag != nullptr) {
        diag("IDXGISwapChain::SetFullscreenState(TRUE) chain=0x%llx "
             "hwnd=0x%llx mode=%ux%u%s",
             static_cast<unsigned long long>(self),
             static_cast<unsigned long long>(hwnd), mode.width, mode.height,
             change_mode ? " (switched)" : " (desktop)");
    }
    return kSOk;
}

void leave_exclusive(DxgiManager& dxgi, const ExclusiveWindowServices& window,
                     GuestAddr self, u64 hwnd, bool flip_model,
                     const StateLookup& lookup, bool leave_host_space,
                     const UnlockedScope& unlocked, DiagFn diag) {
    ExclusiveState* state = lookup();
    if (state == nullptr) return;
    ExclusiveRegistry& registry = dxgi.exclusive_fullscreen();
    const bool was_fullscreen = state->fullscreen;
    const bool mode_changed = state->mode_changed;
    const auto restore = state->windowed_rect;
    const bool leave_space = leave_host_space && state->host_space;
    const GuestAddr output = state->output;
    state->fullscreen = false;
    state->output = 0;
    state->mode_width = 0;
    state->mode_height = 0;
    state->mode_changed = false;
    state->windowed_rect.reset();
    if (flip_model && was_fullscreen) {
        state->needs_resize_buffers = !state->needs_resize_buffers;
    }
    if (leave_space) state->host_space = false;
    GuestAddr expected = self;
    (void)registry.owner.compare_exchange_strong(expected, 0);
    if (output != 0) dxgi.release_dxgi_object(output);
    if (!was_fullscreen && !leave_space) return;
    const auto host = window.host_fullscreen;
    const auto leave = window.leave_exclusive;
    registry.transition = true;
    unlocked([&] {
        if (leave_space && host) host(hwnd, false);
        if (was_fullscreen && leave) leave(hwnd, mode_changed, restore);
    });
    registry.transition = false;
    if (diag != nullptr) {
        diag("IDXGISwapChain left full-screen chain=0x%llx hwnd=0x%llx "
             "space=%d",
             static_cast<unsigned long long>(self),
             static_cast<unsigned long long>(hwnd), leave_space ? 1 : 0);
    }
}

u32 resize_exclusive_target(DxgiManager& dxgi,
                            const ExclusiveWindowServices& window,
                            GuestAddr self, u64 hwnd, u32 flags, u32 width,
                            u32 height, const StateLookup& lookup,
                            const UnlockedScope& unlocked, DiagFn diag) {
    ExclusiveState* state = lookup();
    if (state == nullptr) return kInvalidCall;
    // Without DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH the full-screen
    // resolution "remains the desktop resolution" (Best Practices); the call
    // itself succeeds (measured a73: S_OK, the window and mode unchanged).
    if ((flags & kFlagAllowModeSwitch) == 0) {
        if (diag != nullptr) {
            diag("IDXGISwapChain::ResizeTarget chain=0x%llx full-screen "
                 "without ALLOW_MODE_SWITCH: the desktop mode stays",
                 static_cast<unsigned long long>(self));
        }
        return kSOk;
    }
    const auto info = dxgi.output_info_of(state->output);
    if (!info) return kNotCurrentlyAvailable;
    const auto [native_width, native_height] = output_native_size(*info);
    // "Changes the display mode on the target output when the swap chain is
    // in full-screen mode": the supported mode closest to the request. A
    // request for the current mode "amounts to a no-operation".
    const VirtualDisplayMode mode =
        width == 0 && height == 0
            ? VirtualDisplayMode{state->mode_width, state->mode_height}
            : closest_virtual_display_mode(native_width, native_height, width,
                                           height);
    if (live_diag::caps_log_enabled()) {
        live_diag::emit("dxgi-mode", live_diag::strprintf(
            "ResizeTarget exclusive chain=0x%llx request=%ux%u native=%ux%u selected=%ux%u previous=%ux%u",
            static_cast<unsigned long long>(self), width, height,
            native_width, native_height, mode.width, mode.height,
            state->mode_width, state->mode_height));
    }
    if (mode.width == state->mode_width && mode.height == state->mode_height) {
        return kSOk;
    }
    const bool change_mode =
        mode.width != native_width || mode.height != native_height;
    const std::array<i32, 4> output_rect{
        0, 0, static_cast<i32>(native_width), static_cast<i32>(native_height)};
    const auto enter = window.enter_exclusive;
    if (!enter) return kNotCurrentlyAvailable;
    ExclusiveRegistry& registry = dxgi.exclusive_fullscreen();
    bool entered = false;
    registry.transition = true;
    unlocked([&] {
        entered = enter(hwnd, output_rect, mode.width, mode.height, change_mode);
    });
    registry.transition = false;
    state = lookup();
    if (state == nullptr) return kInvalidCall;
    if (!entered) return kNotCurrentlyAvailable;
    state->mode_width = mode.width;
    state->mode_height = mode.height;
    state->mode_changed = change_mode;
    if (diag != nullptr) {
        diag("IDXGISwapChain::ResizeTarget chain=0x%llx full-screen mode "
             "-> %ux%u", static_cast<unsigned long long>(self), mode.width,
             mode.height);
    }
    return kSOk;
}

void report_fullscreen_state(DxgiManager& dxgi, const ExclusiveState& state,
                             const std::function<void(u32)>& write_bool,
                             const std::function<bool(GuestAddr)>& write_pointer) {
    if (write_bool) write_bool(state.fullscreen ? 1u : 0u);
    if (write_pointer) {
        const bool hand_out = state.fullscreen && state.output != 0 &&
                              dxgi.retain_object(state.output);
        if (!write_pointer(hand_out ? state.output : 0) && hand_out) {
            dxgi.release_dxgi_object(state.output);
        }
    }
}

}  // namespace vitreo::winapi::dxgi
